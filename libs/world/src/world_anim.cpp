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

}  // namespace

bool parse_skinbin(std::span<const uint8_t> data, SpectatorMeshLod& out, int preferred_lod) {
  out.vertices.clear();
  out.indices.clear();
  for (auto& li : out.lod_indices) li.clear();
  out.has_stand_skeleton = false;
  if (data.size() < 0x48) return false;
  const uint8_t* p = data.data();
  const uint32_t num_lods = read_be32(p + 0x04);
  const uint32_t lod0_cnt = read_be32(p + 0x14);
  const uint32_t lod_off[4] = {0u, read_be16(p + 0x1c), read_be16(p + 0x24), read_be16(p + 0x2c)};
  const uint32_t lod_cnt[4] = {lod0_cnt, read_be16(p + 0x1e), read_be16(p + 0x26), read_be16(p + 0x2e)};
  const uint32_t vert_count = read_be32(p + 0x38);
  if (vert_count == 0 || vert_count > 32768 || num_lods == 0 || num_lods > 8) return false;

  const size_t idx_base = 0x40 + size_t(vert_count) * 36;
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
    const uint8_t* vp = p + 0x40 + size_t(i) * 36;
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

  // Detect standard 19-bone FH1 standing spectator skeleton (spine=8, armL=11/12, armR=15/16, head=18).
  out.has_stand_skeleton = (max_bone <= 18u && b_cnt[8] > 0 && b_cnt[11] > 0 && b_cnt[12] > 0 &&
                            b_cnt[15] > 0 && b_cnt[16] > 0 && b_cnt[18] > 0);
  if (out.has_stand_skeleton) {
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
  assets.clear();
  atlas_rgba.clear();

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

      // Load exact 19-bone Standing skeleton hierarchy & bind-pose joint coordinates from
      // anims/Stand_Idle_01.anim.bin (with verified table as fallback).
      stand_skeleton = kDefaultStandSkeleton;
      has_stand_skeleton_data = true;
      if (const auto sit = member_by_low.find("anims/stand_idle_01.anim.bin"); sit != member_by_low.end()) {
        const auto ard = ar->read(members[sit->second]);
        if (ard.ok()) {
          parse_stand_skeleton_from_anim_bin(ard.data, stand_skeleton);
        }
      }

      // Decode ALL 20 standing 3D spectator meshes (.skinbin, LOD0/1/2) + their native 256x256
      // textures (.dds.xds) into a 2048x2048 (8x8 x 256) RGBA8 atlas:
      //   slots  0..19: native 1:1 sRGB diffuse textures
      //   slots 24..43: warm festival/outpost night stage-light bounce (light_map)
      //   slots 43..62: subtle dusk/moonlight ambient fill (light_map)
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
        if (!parse_skinbin(srd.data, asset.mesh, 0)) continue;
        assets.push_back(std::move(asset));
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

namespace {

std::atomic<const drive::Ground*> g_last_ground{nullptr};
std::atomic<const AnimatedWorldSystem*> g_last_ground_owner{nullptr};

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

AnimatedWorldSystem::AnimatedWorldSystem() { populate_default_colorado(); }

AnimatedWorldSystem::~AnimatedWorldSystem() {
  if (g_last_ground_owner.load(std::memory_order_relaxed) == this) {
    g_last_ground.store(nullptr, std::memory_order_relaxed);
    g_last_ground_owner.store(nullptr, std::memory_order_relaxed);
  }
}

const drive::Ground* AnimatedWorldSystem::last_ground() {
  return g_last_ground.load(std::memory_order_relaxed);
}

bool AnimatedWorldSystem::init(const std::filesystem::path& media_dir, const TrackPostConfig& post_cfg,
                               std::string* error) {
  post_cfg_ = post_cfg;
  populate_default_colorado();
  populate_gameobjs_crowds(media_dir);
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

  // Cheering festival spectators positioned strictly behind real Colorado metal festival barriers:
  // obj_fest_barriermetal line from (1992.76, 143.73, 960.09) to (2007.43, 142.78, 949.33),
  // tangent dir = (+0.808, -0.052, -0.589), true orthogonal road-facing normal face = (+0.589, +0.808).
  // Row 1 spans the full metal barrier ~1.10 m behind the rail; Row 2 sits ~0.92 m further back
  // (strictly on the dry dirt shoulder well clear of the lake edge).
  crowds_ = {
      {{1993.65f, 143.65f, 958.25f}, 0.808f, -0.052f, -0.589f, 0.589f, 0.808f, 13, 1.12f, "Barrier", false, true},
      {{1994.75f, 143.52f, 956.40f}, 0.808f, -0.052f, -0.589f, 0.589f, 0.808f, 9, 1.48f, "GigCheer01", false, true},
      // Canonical Horizon Festival Hub baseline clusters (Main Stage, Race Central, Autoshow, Dak's Garage).
      {{-1458.0f, 26.0f, -340.0f}, 0.85f, 0.0f, -0.52f, 0.52f, 0.85f, 12, 1.15f, "GigCheer01", true, true},
      {{-1458.5f, 26.0f, -340.9f}, 0.85f, 0.0f, -0.52f, 0.52f, 0.85f, 8, 1.45f, "Stand", true, true},
      {{-1388.0f, 1.0f, -18.0f}, 0.72f, 0.0f, 0.69f, -0.69f, 0.72f, 12, 1.15f, "Barrier", true, true},
      {{-1387.3f, 1.0f, -18.7f}, 0.72f, 0.0f, 0.69f, -0.69f, 0.72f, 8, 1.45f, "GigCheer01", true, true},
      {{-1012.0f, 0.0f, -252.0f}, 0.92f, 0.0f, 0.39f, -0.39f, 0.92f, 10, 1.20f, "Barrier", true, true},
      {{-952.0f, 0.0f, -172.0f}, 0.60f, 0.0f, -0.80f, 0.80f, 0.60f, 10, 1.20f, "Barrier", true, true},
  };
}

void AnimatedWorldSystem::populate_gameobjs_crowds(const std::filesystem::path& media_dir) {
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
    for (const auto& c : crowds_) {
      const float dx = c.origin[0] - x;
      const float dz = c.origin[2] - z;
      if (dx * dx + dz * dz < min_d2) return true;
    }
    return false;
  };

  size_t pos = 0;
  constexpr size_t kMaxAutoClusters = 96;
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
    const int count = is_festival ? 10 : (is_outpost ? 9 : 7);
    const float spacing = is_festival ? 1.15f : 1.18f;
    const float half_span = 0.5f * float(count - 1) * spacing;
    const float cx = ox + xx * lateral - zx * half_span;
    const float cz = oz + xz * lateral - zz * half_span;
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
    crowds_.push_back(std::move(cl));
    ++added;

    // Add a staggered second row behind Festival/Outpost/Exhibition crowds for authentic depth.
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
      back.crowdclass = "GigCheer01";
      back.require_offroad = true;
      back.stage_lit = lit_stage;
      crowds_.push_back(std::move(back));
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

  // Authentic Colorado props (Horizon festival banners, fences, structures) are streamed from
  // FH1 .rmb.bin assets with GPU wind sway (world_vert.glsl). Synthetic procedural windmills,
  // tri-stripe flags, and blocky box spectators are intentionally omitted so they do not
  // overlay authentic Colorado scenery.
  (void)wx;
  (void)wz;
  (void)ground_y;

  // 2. Roadside wind-swaying grass quads (_0x00005007 / TrackSettings grass parameters).
  if (include_grass && ground && post_cfg_.grass_height_scale > 0.01f) {
    const float max_r = std::clamp(post_cfg_.grass_fade_end * 0.20f, 22.0f, 52.0f);
    const float fade_start = std::clamp(post_cfg_.grass_fade_start * 0.14f, 10.0f, max_r - 4.0f);
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

void AnimatedWorldSystem::build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                               const drive::Ground* ground, std::vector<SpectatorVertex>& out) const {
  build_spectator_mesh(eye_x, eye_y, eye_z, time_sec, ground, out, nullptr);
}

void AnimatedWorldSystem::build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                               const drive::Ground* ground, std::vector<SpectatorVertex>& out,
                                               std::vector<std::array<float, 4>>* shadow_discs) const {
  (void)eye_y;
  if (!catalog_.has_3d_spectators() || crowds_.empty()) return;
  const float inv_grid = 1.0f / float(SpectatorCatalog::kAtlasGrid);
  const float uv_pad = 1.0f / float(SpectatorCatalog::kAtlasCell);
  const auto& skel = catalog_.has_stand_skeleton_data ? catalog_.stand_skeleton : kDefaultStandSkeleton;

  struct GroundSample {
    float y = 0.0f;
    bool hit = false;
    bool blocked = false;
    bool on_road = false;
    bool steep = false;
  };
  auto sample_ground = [&](float x, float z, float fallback) -> GroundSample {
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

  // Pre-build male-only and full asset index pools for crowdclass variety.
  std::vector<size_t> all_pool;
  std::vector<size_t> male_pool;
  all_pool.reserve(catalog_.assets.size());
  male_pool.reserve(catalog_.assets.size());
  for (size_t a = 0; a < catalog_.assets.size(); ++a) {
    all_pool.push_back(a);
    if (catalog_.assets[a].is_male) male_pool.push_back(a);
  }
  if (male_pool.empty()) male_pool = all_pool;

  // Sort nearby clusters closest-first so high-detail near spectators are never truncated by GPU vertex cap.
  struct ClusterRef {
    size_t idx;
    double dist2;
  };
  std::vector<ClusterRef> active_clusters;
  active_clusters.reserve(16);
  for (size_t ci = 0; ci < crowds_.size(); ++ci) {
    const auto& cl = crowds_[ci];
    const double mid = 0.5 * double(std::max(0, cl.count - 1)) * double(cl.spacing);
    const double cdx = double(cl.origin[0]) + double(cl.dir_x) * mid - eye_x;
    const double cdz = double(cl.origin[2]) + double(cl.dir_z) * mid - eye_z;
    const double d2 = cdx * cdx + cdz * cdz;
    if (d2 <= 145.0 * 145.0) active_clusters.push_back({ci, d2});
  }
  std::sort(active_clusters.begin(), active_clusters.end(),
            [](const ClusterRef& a, const ClusterRef& b) { return a.dist2 < b.dist2; });

  auto slot_uv_off = [&](uint32_t slot) -> std::array<float, 2> {
    return {float(slot % SpectatorCatalog::kAtlasGrid) * inv_grid,
            float(slot / SpectatorCatalog::kAtlasGrid) * inv_grid};
  };

  for (const auto& cref : active_clusters) {
    const size_t ci = cref.idx;
    const auto& cl = crowds_[ci];
    const std::string low_cc = lower_ascii(cl.crowdclass);
    const bool is_gig = (low_cc.find("gig") != std::string::npos || low_cc.find("cheer") != std::string::npos);
    const bool is_male_only = (low_cc.find("sat") != std::string::npos && low_cc.find("02") == std::string::npos &&
                               low_cc.find("03") == std::string::npos);
    const auto& pool = is_male_only ? male_pool : all_pool;
    const size_t pool_n = pool.size();

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
      const GroundSample gs = sample_ground(px, pz, cl.origin[1] + cl.dir_y * along);
      if (gs.hit && (gs.blocked || gs.steep || (cl.require_offroad && gs.on_road))) continue;
      const float py = gs.y;

      const double dx = double(px) - eye_x;
      const double dz = double(pz) - eye_z;
      const double d2 = dx * dx + dz * dz;
      if (d2 > 120.0 * 120.0) continue;
      const float dist = float(std::sqrt(d2));

      const auto& asset = catalog_.assets[pool[(start_asset + size_t(i) * stride) % pool_n]];
      const auto diff_off = slot_uv_off(asset.atlas_slot);
      const auto lm_off = slot_uv_off(cl.stage_lit ? SpectatorCatalog::kNightLitSlot : SpectatorCatalog::kNightDimSlot);

      const float yaw = base_yaw + (r2 - 0.5f) * 0.38f;
      const float cyaw = std::cos(yaw);
      const float syaw = std::sin(yaw);
      // Natural human height variation (~1.68 m to 1.86 m for FH1's ~1.79 m rigs).
      const float scale = asset.is_male ? (0.96f + 0.08f * r0) : (0.93f + 0.07f * r0);

      // Subtle living head gaze tracking toward the player car/camera when passing within 35 m in front.
      float gaze_yaw = 0.0f;
      if (dist > 1.5f && dist < 35.0f) {
        const float to_ex = float(-dx) / dist;
        const float to_ez = float(-dz) / dist;
        const float fwd_dot = to_ex * syaw + to_ez * cyaw;
        if (fwd_dot > 0.15f) {
          // Local +X is character left = (cyaw, 0, -syaw).
          const float left_dot = to_ex * cyaw - to_ez * syaw;
          const float rel_ang = std::atan2(left_dot, fwd_dot);
          const float w_dist = std::clamp((35.0f - dist) / 22.0f, 0.0f, 1.0f);
          const float w_front = std::clamp((fwd_dot - 0.15f) / 0.45f, 0.0f, 1.0f);
          gaze_yaw = std::clamp(rel_ang, -0.52f, 0.52f) * w_dist * w_front;
        }
      }

      // Per-spectator tempo, phase, handedness, and slow energy envelope so crowds feel organic & alive.
      const float phase = r1 * 6.2831853f;
      const float freq = (is_gig ? 3.2f : 2.5f) + 1.2f * r2;
      const float env = 0.65f + 0.35f * std::sin(time_sec * (0.38f + 0.25f * r3) + phase * 2.1f);
      const float beat = std::sin(time_sec * freq + phase);
      const float beat2 = std::sin(time_sec * (freq * 2.0f) + phase * 1.5f);
      const float sway = std::cos(time_sec * (freq * 0.5f) + phase * 1.3f);
      const float idle_look = std::sin(time_sec * (0.65f + 0.35f * r3) + phase * 1.7f) * 0.22f;
      const bool mirror = ((h3 >> 16) & 1u) != 0u;

      // Choose from 8 authentic FH1 festival poses (balanced candid festival mix: ~45% cheering/waving,
      // ~30% clapping/shoulder fist-pump/filming, ~25% barrier-lean/relaxed groove).
      const uint32_t style_roll = (h0 >> 16) % 10u;
      uint32_t style = 5u;
      if (is_gig) {
        static constexpr uint32_t kGigStyles[10] = {0, 1, 2, 3, 6, 1, 2, 6, 7, 5};
        style = kGigStyles[style_roll];
      } else {
        static constexpr uint32_t kBarrierStyles[10] = {1, 2, 4, 5, 6, 7, 0, 4, 5, 3};
        style = kBarrierStyles[style_roll];
      }

      // Build exact 19-bone hierarchical Forward Kinematics transforms (zero tearing, watertight joints).
      std::array<BoneXform, SpectatorMeshLod::kMaxBones> bone_xf{};
      if (asset.mesh.has_stand_skeleton) {
        std::array<Mat3, SpectatorCatalog::kStandBoneCount> local_r{};
        for (auto& m : local_r) m = mat3_id();

        float hip_x = sway * 0.018f;
        float hip_z = 0.0f;
        float knee_bend = 0.04f + 0.05f * (0.5f + 0.5f * beat) * env;
        float spine_pitch = 0.04f;  // + = lean forward toward road
        float spine_yaw = 0.06f * sway;
        float spine_roll = 0.03f * sway;
        float head_pitch = -0.02f + 0.04f * beat * env;
        float head_yaw = gaze_yaw + idle_look * (1.0f - std::min(std::fabs(gaze_yaw) * 1.4f, 0.8f));

        // Left & right arm anatomical parameters (natural relaxed defaults — NEVER stiff T/A-pose).
        float l_up = 0.14f, l_out = 0.07f, l_twist = 0.18f, l_elbow = 0.42f, l_wrist_z = 0.0f;
        float r_up = 0.14f, r_out = 0.07f, r_twist = 0.18f, r_elbow = 0.42f, r_wrist_z = 0.0f;

        if (style == 0u) {
          // 0: Enthusiastic double-arm concert cheer with naturally bent elbows (not stiff straight-up).
          const float pump = 0.22f * beat * env;
          l_up = 2.15f + pump;
          l_out = 0.36f + 0.08f * sway;
          l_twist = 0.32f;
          l_elbow = 1.02f - 0.26f * beat * env;
          r_up = 2.10f - pump * 0.7f;
          r_out = 0.34f - 0.08f * sway;
          r_twist = 0.32f;
          r_elbow = 1.06f + 0.22f * beat * env;
          spine_pitch = -0.04f + 0.04f * beat * env;
          head_pitch = -0.08f + 0.05f * beat * env;
          knee_bend = 0.06f + 0.08f * (0.5f + 0.5f * beat) * env;
        } else if (style == 1u) {
          // 1: One-arm concert fist pump + relaxed hip/side off-arm.
          const float pump = 0.28f * beat * env;
          l_up = 2.18f + pump;
          l_out = 0.28f + 0.06f * sway;
          l_twist = 0.34f;
          l_elbow = 1.12f - 0.34f * beat * env;
          r_up = 0.10f;
          r_out = 0.15f;
          r_twist = 0.35f;
          r_elbow = 0.65f + 0.08f * sway;
          spine_pitch = 0.02f - 0.03f * beat * env;
          spine_yaw = -0.10f + 0.08f * sway;
          head_pitch = -0.05f + 0.04f * beat * env;
          knee_bend = 0.05f + 0.07f * (0.5f + 0.5f * beat) * env;
        } else if (style == 2u) {
          // 2: Rhythmic chest-height festival clapping (hands meet cleanly in front of sternum!).
          const float clap = 0.5f + 0.5f * beat2;  // 0 = hands together, 1 = hands apart
          l_up = 0.54f + 0.06f * beat * env;
          l_out = 0.06f + 0.14f * clap * env;
          l_twist = 0.78f - 0.18f * clap * env;
          l_elbow = 1.58f - 0.10f * clap * env;
          r_up = 0.54f + 0.06f * beat * env;
          r_out = 0.06f + 0.14f * clap * env;
          r_twist = 0.78f - 0.18f * clap * env;
          r_elbow = 1.58f - 0.10f * clap * env;
          spine_pitch = 0.08f + 0.03f * beat * env;
          knee_bend = 0.05f + 0.06f * clap * env;
        } else if (style == 3u) {
          // 3: Overhead two-hand side-to-side concert wave (soft bent elbows).
          const float wave = 0.24f * sway * env;
          l_up = 2.28f + 0.08f * beat;
          l_out = 0.30f + wave;
          l_twist = 0.24f;
          l_elbow = 0.78f + 0.14f * std::fabs(sway);
          l_wrist_z = wave * 0.55f;
          r_up = 2.28f - 0.08f * beat;
          r_out = 0.30f - wave;
          r_twist = 0.24f;
          r_elbow = 0.78f + 0.14f * std::fabs(sway);
          r_wrist_z = -wave * 0.55f;
          spine_roll = 0.09f * sway * env;
          hip_x = 0.028f * sway;
        } else if (style == 4u) {
          // 4: Barrier lean & watch (torso slightly forward, forearms bent naturally, periodic wave).
          const float cheer_burst = std::max(0.0f, std::sin(time_sec * 0.55f + phase * 1.9f));
          spine_pitch = 0.15f - 0.06f * cheer_burst;
          hip_z = 0.02f;
          l_up = 0.34f + 1.65f * cheer_burst;
          l_out = 0.12f + 0.16f * cheer_burst;
          l_twist = 0.40f - 0.10f * cheer_burst;
          l_elbow = 1.08f - 0.15f * cheer_burst + 0.18f * beat * cheer_burst;
          r_up = 0.34f;
          r_out = 0.12f;
          r_twist = 0.42f;
          r_elbow = 1.05f + 0.08f * sway;
          knee_bend = 0.07f;
        } else if (style == 5u) {
          // 5: Relaxed festival groove & sway (natural standing weight shift, soft bent elbows).
          l_up = 0.14f + 0.07f * sway;
          l_out = 0.08f;
          l_twist = 0.26f;
          l_elbow = 0.48f + 0.10f * beat;
          r_up = 0.14f - 0.07f * sway;
          r_out = 0.08f;
          r_twist = 0.26f;
          r_elbow = 0.48f - 0.10f * beat;
          spine_pitch = 0.05f;
          spine_yaw = 0.12f * sway;
          spine_roll = 0.05f * sway;
          hip_x = 0.026f * sway;
          knee_bend = 0.04f + 0.06f * (0.5f + 0.5f * beat);
        } else if (style == 6u) {
          // 6: Energetic festival bounce / dual shoulder-height fist pump!
          const float bounce = 0.5f + 0.5f * beat;
          l_up = 1.68f + 0.34f * beat * env;
          l_out = 0.28f + 0.06f * sway;
          l_twist = 0.26f;
          l_elbow = 1.48f - 0.32f * beat * env;
          r_up = 1.64f + 0.32f * beat * env;
          r_out = 0.28f - 0.06f * sway;
          r_twist = 0.26f;
          r_elbow = 1.48f - 0.30f * beat * env;
          spine_pitch = 0.04f - 0.05f * beat * env;
          knee_bend = 0.04f + 0.20f * (1.0f - bounce) * env;
        } else {
          // 7: Holding phone / camera up to film passing cars + casual sway!
          l_up = 0.92f + 0.05f * sway;
          l_out = 0.08f;
          l_twist = 0.72f;
          l_elbow = 1.72f;
          r_up = 0.88f - 0.04f * sway;
          r_out = 0.08f;
          r_twist = 0.72f;
          r_elbow = 1.70f;
          spine_yaw = gaze_yaw * 0.45f + 0.12f * sway;
          spine_pitch = 0.06f;
          head_yaw = gaze_yaw * 0.55f + 0.08f * sway;
          knee_bend = 0.05f;
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

        // Exact foot-planted knee bounce vertical drop: pelvis drops strictly as knees flex.
        const float thigh_a = knee_bend * 0.55f;
        const float hip_dy = -0.82f * (1.0f - std::cos(thigh_a));

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
        const float l_shrug = std::clamp((l_up - 1.1f) / 1.6f, 0.0f, 1.0f) * 0.22f;
        const float r_shrug = std::clamp((r_up - 1.1f) / 1.6f, 0.0f, 1.0f) * 0.22f;
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
          bind_p[b] = {skel[b].bind_world[0], skel[b].bind_world[1] - asset.mesh.min_y, skel[b].bind_world[2]};
          const int16_t par = skel[b].parent;
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

      // Select crisp multi-LOD mesh by distance: LOD 0 (<32 m), LOD 1 (32..68 m), LOD 2 (>68 m).
      const size_t lod_idx = (dist < 32.0f) ? 0u : ((dist < 68.0f) ? 1u : 2u);
      const auto& verts = asset.mesh.vertices;
      const auto& inds =
          !asset.mesh.lod_indices[lod_idx].empty() ? asset.mesh.lod_indices[lod_idx] : asset.mesh.indices;

      if (shadow_discs) {
        shadow_discs->push_back({px, py + 0.03f, pz, 0.42f * scale});
      }

      out.reserve(out.size() + inds.size());
      for (uint16_t idx : inds) {
        const auto& v = verts[idx];
        // Normalize feet strictly to y=0 so every spectator stands flush on the terrain surface.
        const std::array<float, 3> bp{v.pos[0], v.pos[1] - asset.mesh.min_y, v.pos[2]};
        std::array<float, 3> posed_p = bp;
        std::array<float, 3> posed_n = v.normal;

        if (asset.mesh.has_stand_skeleton && v.bone0 < SpectatorCatalog::kStandBoneCount) {
          const float w0 = float(v.weight0);
          const float w1 = (v.bone1 < SpectatorCatalog::kStandBoneCount) ? float(v.weight1) : 0.0f;
          const float wsum = w0 + w1;
          if (wsum > 1e-3f) {
            const float a0 = w0 / wsum;
            const float a1 = w1 / wsum;
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
          // Fallback upper-body sway for synthetic meshes without the 19-bone rig.
          const float upper = std::clamp((bp[1] - 0.85f) / 0.85f, 0.0f, 1.0f);
          const float mid = std::clamp((bp[1] - 0.12f) / 0.60f, 0.0f, 1.0f);
          posed_p[0] += sway * 0.028f * upper;
          posed_p[1] += (0.5f + 0.5f * beat) * 0.020f * mid;
          posed_p[2] += beat * 0.022f * upper;
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
        const float lm_u = lm_off[0] + 0.5f * inv_grid;
        const float lm_v = lm_off[1] + h_norm * inv_grid;
        out.push_back({{wx, wy, wz}, {nx, ny, nz}, {u, tv}, {255, 255, 255, 255}, {lm_u, lm_v}});
      }
    }
  }
}

}  // namespace world
