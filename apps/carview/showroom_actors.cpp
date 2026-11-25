#include "showroom_internal.h"

#include "cars/mods.h"
#include "fh1data/archive.h"
#include "fh1data/xds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace carview {
namespace {

struct DriverPoseParams {
  std::array<float, 3> pivot{-0.36f, 0.72f, 0.08f};
  std::array<float, 3> axis{0.0f, -0.25f, -0.968f};
  float rim_r = 0.165f;
  float wheel_rad = 0.0f;
  float shift_blend = 0.0f;
  float handbrake_blend = 0.0f;
  float hand_scale = 1.0f;
  bool clamp_roof = false;
  float roof_max_y = 0.0f;
};

struct DriverEmitResult {
  uint32_t hands_count = 0;
  uint32_t total_count = 0;
  float head_above_pivot_y = 0.33f;
};

// Per-Showroom static neutral seated driver vertex buffer for AI/traffic guest cars
// so guest cars never mirror the player's live steering/shift pose or LHD/RHD seat.
struct GuestDriverCache {
  Renderer::Buffer vb;
  void* mapped = nullptr;
  uint32_t vert_count = 0;
  float head_above_pivot_y = 0.33f;
  bool built = false;
};

std::mutex g_guest_driver_mutex;
std::unordered_map<const Showroom*, std::unique_ptr<GuestDriverCache>> g_guest_driver_caches;

DriverEmitResult emit_driver_geometry(const world::SpectatorMeshLod* skin_mesh, const DriverPoseParams& p,
                                      WorldVertex* dst, uint32_t max_verts) {
  DriverEmitResult res{};
  if (!dst || max_verts < 3u) return res;

  auto vadd = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
  };
  auto vsub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  };
  auto vscale = [](const std::array<float, 3>& a, float s) -> std::array<float, 3> {
    return {a[0] * s, a[1] * s, a[2] * s};
  };
  auto vlerp = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float t) -> std::array<float, 3> {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
  };
  auto vdot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> float {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  auto vcross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::array<float, 3> {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  };
  auto vnorm = [](const std::array<float, 3>& a) -> std::array<float, 3> {
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0, 1, 0};
  };
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };

  uint32_t count = 0;
  auto push_tri = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                      const std::array<uint8_t, 4>& col) {
    if (count + 3u > max_verts) return;
    // world_opaque_ uses VK_FRONT_FACE_CLOCKWISE (for det=-1 world_transform_); emit CW order
    // (a, c, b) while keeping the outward CCW geometric normal so car-space det=+1 faces draw.
    const auto n = vnorm(vcross(vsub(b, a), vsub(c, a)));
    const std::array<float, 3>* pts[3] = {&a, &c, &b};
    for (int k = 0; k < 3; ++k) {
      WorldVertex& o = dst[count++];
      o = WorldVertex{};
      o.position[0] = (*pts[k])[0];
      o.position[1] = (*pts[k])[1];
      o.position[2] = (*pts[k])[2];
      o.normal[0] = snorm8(n[0]);
      o.normal[1] = snorm8(n[1]);
      o.normal[2] = snorm8(n[2]);
      for (int j = 0; j < 4; ++j) o.colour[j] = col[j];
    }
  };
  auto push_quad = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c,
                       const std::array<float, 3>& d, const std::array<uint8_t, 4>& col) {
    push_tri(a, b, c, col);
    push_tri(a, c, d, col);
  };
  auto push_box = [&](const std::array<float, 3>& c, const std::array<float, 3>& rx, const std::array<float, 3>& ry,
                      const std::array<float, 3>& rz, const std::array<uint8_t, 4>& col) {
    auto pt = [&](float sx, float sy, float sz) -> std::array<float, 3> {
      return {c[0] + rx[0] * sx + ry[0] * sy + rz[0] * sz, c[1] + rx[1] * sx + ry[1] * sy + rz[1] * sz,
              c[2] + rx[2] * sx + ry[2] * sy + rz[2] * sz};
    };
    const auto p000 = pt(-1, -1, -1), p100 = pt(1, -1, -1), p110 = pt(1, 1, -1), p010 = pt(-1, 1, -1);
    const auto p001 = pt(-1, -1, 1), p101 = pt(1, -1, 1), p111 = pt(1, 1, 1), p011 = pt(-1, 1, 1);
    push_quad(p001, p101, p111, p011, col);
    push_quad(p100, p000, p010, p110, col);
    push_quad(p011, p111, p110, p010, col);
    push_quad(p000, p100, p101, p001, col);
    push_quad(p101, p100, p110, p111, col);
    push_quad(p000, p001, p011, p010, col);
  };
  auto push_limb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float rad,
                       const std::array<uint8_t, 4>& col) {
    const auto dir = vsub(b, a);
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-4f) return;
    const auto f = vscale(dir, 0.5f);
    const auto mid = vadd(a, f);
    const auto u = vnorm(dir);
    const std::array<float, 3> ref = std::fabs(u[1]) < 0.9f ? std::array<float, 3>{0, 1, 0} : std::array<float, 3>{1, 0, 0};
    const auto r1 = vscale(vnorm(vcross(ref, u)), rad);
    const auto r2 = vscale(vnorm(vcross(u, r1)), rad);
    push_box(mid, r1, r2, f, col);
  };

  // Orthonormal wheel plane basis perpendicular to steering column axis.
  const auto ax = vnorm(p.axis);
  const auto wheel_right = vnorm(vcross({0.0f, 1.0f, 0.0f}, vscale(ax, -1.0f)));
  const auto wheel_up = vnorm(vcross(vscale(ax, -1.0f), wheel_right));

  // Hand-over-hand steering animation from 0 to +/-270 deg (+/-4.712 rad).
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kCrossoverRad = 2.3561945f;  // 135 deg
  constexpr float kLockRad = 4.7123890f;       // 270 deg
  const float w_clamped = std::clamp(p.wheel_rad, -kLockRad, kLockRad);
  float l_ang = w_clamped;
  float r_ang = w_clamped;
  float l_lift = 0.0f;
  float r_lift = 0.0f;
  if (w_clamped > kCrossoverRad) {
    const float ex = std::clamp((w_clamped - kCrossoverRad) / (kLockRad - kCrossoverRad), 0.0f, 1.0f);
    const float smooth_ex = ex * ex * (3.0f - 2.0f * ex);
    l_ang = kCrossoverRad + (w_clamped - kCrossoverRad) * 0.48f;
    r_ang = kCrossoverRad + (w_clamped - kPi - kCrossoverRad) * smooth_ex;
    r_lift = std::sin(kPi * ex) * 0.048f;
  } else if (w_clamped < -kCrossoverRad) {
    const float ex = std::clamp((-w_clamped - kCrossoverRad) / (kLockRad - kCrossoverRad), 0.0f, 1.0f);
    const float smooth_ex = ex * ex * (3.0f - 2.0f * ex);
    r_ang = -kCrossoverRad + (w_clamped + kCrossoverRad) * 0.48f;
    l_ang = -kCrossoverRad + (w_clamped + kPi + kCrossoverRad) * smooth_ex;
    l_lift = std::sin(kPi * ex) * 0.048f;
  }
  const auto l_dir = vadd(vscale(wheel_right, -std::cos(l_ang)), vscale(wheel_up, std::sin(l_ang)));
  const auto r_dir = vadd(vscale(wheel_right, std::cos(r_ang)), vscale(wheel_up, -std::sin(r_ang)));
  std::array<float, 3> l_hand = vadd(vadd(p.pivot, vscale(l_dir, p.rim_r)), vscale(ax, -l_lift));
  std::array<float, 3> r_hand = vadd(vadd(p.pivot, vscale(r_dir, p.rim_r)), vscale(ax, -r_lift));

  // Smooth gear-shift reach and handbrake pull animations on the center-console hand (right on LHD, left on RHD).
  const bool rhd = p.pivot[0] > 0.05f;
  auto& console_hand = rhd ? l_hand : r_hand;
  if (p.handbrake_blend > 0.01f) {
    const float hb = p.handbrake_blend;
    const float s_hb = hb * hb * (3.0f - 2.0f * hb);
    const std::array<float, 3> hb_target{rhd ? -0.04f : 0.04f, p.pivot[1] - 0.21f + 0.055f * s_hb, p.pivot[2] + 0.27f};
    console_hand = vlerp(console_hand, hb_target, s_hb);
    console_hand[1] += std::sin(kPi * s_hb) * 0.025f;
  } else if (p.shift_blend > 0.01f) {
    const float sh = p.shift_blend;
    const float s_sh = sh * sh * (3.0f - 2.0f * sh);
    const std::array<float, 3> shift_target{0.0f, p.pivot[1] - 0.23f, p.pivot[2] + 0.16f};
    console_hand = vlerp(console_hand, shift_target, s_sh);
    console_hand[1] += std::sin(kPi * s_sh) * 0.032f;
  }

  // Seated festival driver behind steering wheel (+Z rearward in model space).
  const float lean = std::clamp(w_clamped * 0.024f, -0.055f, 0.055f);
  const std::array<float, 3> chest{p.pivot[0] + lean * 0.4f, p.pivot[1] - 0.06f, p.pivot[2] + 0.36f};
  const std::array<float, 3> head{p.pivot[0] + lean, p.pivot[1] + 0.21f, p.pivot[2] + 0.38f};
  const std::array<float, 3> l_sh{chest[0] - 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> r_sh{chest[0] + 0.21f, chest[1] + 0.14f, chest[2] - 0.02f};
  const std::array<float, 3> l_el{(l_sh[0] + l_hand[0]) * 0.5f - 0.06f, (l_sh[1] + l_hand[1]) * 0.5f - 0.08f,
                                  (l_sh[2] + l_hand[2]) * 0.5f + 0.04f};
  const std::array<float, 3> r_el{(r_sh[0] + r_hand[0]) * 0.5f + 0.06f, (r_sh[1] + r_hand[1]) * 0.5f - 0.08f,
                                  (r_sh[2] + r_hand[2]) * 0.5f + 0.04f};

  // Emit authentic skinned seated driver mesh from media/cars/Driver.zip (`Driver01.skinbin`)
  // textured with `Driver01_D.xds` when loaded.
  if (skin_mesh && !skin_mesh->vertices.empty() && skin_mesh->indices.size() >= 3) {
    const auto& mesh = *skin_mesh;
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f, min_z = 1e9f, max_z = -1e9f;
    std::array<double, 3> lh_sum{0, 0, 0}, rh_sum{0, 0, 0}, head_sum{0, 0, 0};
    uint32_t lh_cnt = 0, rh_cnt = 0, head_cnt = 0;
    float head_max_y = -1e9f;
    float lh_min_z = 1e9f, lh_max_z = -1e9f;
    float rh_min_z = 1e9f, rh_max_z = -1e9f;
    for (const auto& v : mesh.vertices) {
      min_x = std::min(min_x, v.pos[0]); max_x = std::max(max_x, v.pos[0]);
      min_y = std::min(min_y, v.pos[1]); max_y = std::max(max_y, v.pos[1]);
      min_z = std::min(min_z, v.pos[2]); max_z = std::max(max_z, v.pos[2]);
      if (v.bone0 == 5) {
        lh_sum[0] += v.pos[0]; lh_sum[1] += v.pos[1]; lh_sum[2] += v.pos[2];
        ++lh_cnt;
        lh_min_z = std::min(lh_min_z, v.pos[2]);
        lh_max_z = std::max(lh_max_z, v.pos[2]);
      } else if (v.bone0 == 8) {
        rh_sum[0] += v.pos[0]; rh_sum[1] += v.pos[1]; rh_sum[2] += v.pos[2];
        ++rh_cnt;
        rh_min_z = std::min(rh_min_z, v.pos[2]);
        rh_max_z = std::max(rh_max_z, v.pos[2]);
      } else if (v.bone0 == 2) {
        head_sum[0] += v.pos[0]; head_sum[1] += v.pos[1]; head_sum[2] += v.pos[2];
        ++head_cnt;
        head_max_y = std::max(head_max_y, v.pos[1]);
      }
    }
    const float src_cx = (head_cnt > 0) ? float(head_sum[0] / double(head_cnt)) : 0.5f * (min_x + max_x);
    const std::array<float, 3> src_lh =
        (lh_cnt > 0) ? std::array<float, 3>{float(lh_sum[0] / lh_cnt), float(lh_sum[1] / lh_cnt), lh_min_z + 0.08f}
                     : std::array<float, 3>{src_cx - 0.19f, min_y + 0.56f * (max_y - min_y), min_z + 0.42f};
    const std::array<float, 3> src_rh =
        (rh_cnt > 0) ? std::array<float, 3>{float(rh_sum[0] / rh_cnt), float(rh_sum[1] / rh_cnt), rh_min_z + 0.08f}
                     : std::array<float, 3>{src_cx + 0.19f, min_y + 0.56f * (max_y - min_y), min_z + 0.42f};
    const float src_hand_y = 0.5f * (src_lh[1] + src_rh[1]);
    const float src_hand_z = 0.5f * (src_lh[2] + src_rh[2]);
    const float src_top_y = (head_cnt > 0) ? head_max_y : max_y;

    const float scale = 0.92f;
    res.head_above_pivot_y = std::max(0.20f, (src_top_y - src_hand_y) * scale);
    float seat_dy = 0.0f;
    if (p.clamp_roof) {
      const float head_top_y = p.pivot[1] + res.head_above_pivot_y;
      const float roof_limit = p.roof_max_y - 0.075f;
      if (head_top_y > roof_limit) {
        seat_dy = -std::min(head_top_y - roof_limit, 0.12f);
      }
    }
    auto map_rest = [&](const std::array<float, 3>& pt) -> std::array<float, 3> {
      return {
          p.pivot[0] + (pt[0] - src_cx) * scale,
          p.pivot[1] + seat_dy + (pt[1] - src_hand_y) * scale,
          p.pivot[2] + (pt[2] - src_hand_z) * scale,
      };
    };
    const auto rest_lh = map_rest(src_lh);
    const auto rest_rh = map_rest(src_rh);
    auto clamp_vec = [&](const std::array<float, 3>& v, float max_len) -> std::array<float, 3> {
      const float l = std::sqrt(vdot(v, v));
      return (l > max_len && l > 1e-6f) ? vscale(v, max_len / l) : v;
    };
    const auto dl = clamp_vec(vsub(l_hand, rest_lh), 0.28f);
    const auto dr = clamp_vec(vsub(r_hand, rest_rh), 0.28f);
    const float lh_span_z = std::max(lh_max_z - lh_min_z, 0.12f);
    const float rh_span_z = std::max(rh_max_z - rh_min_z, 0.12f);

    auto bone_disp = [&](uint8_t bone, const std::array<float, 3>& sp) -> std::array<float, 3> {
      switch (bone) {
        case 5: {
          const float t = std::clamp((lh_max_z - sp[2]) / lh_span_z, 0.0f, 1.0f);
          return vscale(dl, 0.42f + 0.58f * t);
        }
        case 4: return vscale(dl, 0.36f);
        case 3: return vscale(dl, 0.10f);
        case 8: {
          const float t = std::clamp((rh_max_z - sp[2]) / rh_span_z, 0.0f, 1.0f);
          return vscale(dr, 0.42f + 0.58f * t);
        }
        case 7: return vscale(dr, 0.36f);
        case 6: return vscale(dr, 0.10f);
        case 2: return {lean, 0.0f, 0.0f};
        case 1: return {lean * 0.45f, 0.0f, 0.0f};
        default: return {0.0f, 0.0f, 0.0f};
      }
    };

    auto is_arm_bone = [](uint8_t b) { return b == 4 || b == 5 || b == 7 || b == 8; };
    auto emit_pass = [&](bool arms_only) {
      const size_t n_idx = (mesh.indices.size() / 3u) * 3u;
      for (size_t i = 0; i < n_idx; i += 3) {
        if (count + 3u > max_verts) break;
        const uint16_t i0 = mesh.indices[i + 0];
        const uint16_t i1 = mesh.indices[i + 1];
        const uint16_t i2 = mesh.indices[i + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;
        const bool tri_is_arm = is_arm_bone(mesh.vertices[i0].bone0) && is_arm_bone(mesh.vertices[i1].bone0) &&
                                is_arm_bone(mesh.vertices[i2].bone0);
        if (tri_is_arm != arms_only) continue;
        // Positive-determinant transform in car space; flip parse_skinbin CCW -> CW for world_opaque_.
        const uint16_t tri[3] = {i0, i2, i1};
        for (int k = 0; k < 3; ++k) {
          const auto& sv = mesh.vertices[tri[k]];
          const auto base_p = map_rest(sv.pos);
          const float w0 = float(sv.weight0) / 255.0f;
          const float w1 = float(sv.weight1) / 255.0f;
          const float wsum = w0 + w1;
          std::array<float, 3> disp{0.0f, 0.0f, 0.0f};
          if (wsum > 1e-4f) {
            const auto d0 = bone_disp(sv.bone0, sv.pos);
            const auto d1 = (w1 > 0.0f) ? bone_disp(sv.bone1, sv.pos) : std::array<float, 3>{0, 0, 0};
            disp = vscale(vadd(vscale(d0, w0), vscale(d1, w1)), 1.0f / wsum);
          } else {
            disp = bone_disp(sv.bone0, sv.pos);
          }
          WorldVertex& o = dst[count++];
          o = WorldVertex{};
          o.position[0] = base_p[0] + disp[0];
          o.position[1] = base_p[1] + disp[1];
          o.position[2] = base_p[2] + disp[2];
          o.normal[0] = snorm8(sv.normal[0]);
          o.normal[1] = snorm8(sv.normal[1]);
          o.normal[2] = snorm8(sv.normal[2]);
          o.uv0[0] = sv.uv[0];
          o.uv0[1] = sv.uv[1];
          o.colour[0] = o.colour[1] = o.colour[2] = o.colour[3] = 255;
        }
      }
    };

    emit_pass(true);
    res.hands_count = count;
    emit_pass(false);
    res.total_count = count;
    if (res.total_count > 0) return res;
  }

  // Improved procedural fallback driver when media/cars/Driver.zip is absent.
  auto push_ellipsoid = [&](const std::array<float, 3>& c, float rx, float ry, float rz,
                            const std::array<uint8_t, 4>& col, int stacks = 8, int slices = 14) {
    auto pt = [&](int st, int sl) -> std::array<float, 3> {
      const float phi = -0.5f * kPi + kPi * float(st) / float(stacks);
      const float th = 2.0f * kPi * float(sl) / float(slices);
      const float cp = std::cos(phi);
      return {c[0] + rx * cp * std::cos(th), c[1] + ry * std::sin(phi), c[2] + rz * cp * std::sin(th)};
    };
    for (int st = 0; st < stacks; ++st) {
      for (int sl = 0; sl < slices; ++sl) {
        const auto p00 = pt(st, sl);
        const auto p10 = pt(st + 1, sl);
        const auto p11 = pt(st + 1, sl + 1);
        const auto p01 = pt(st, sl + 1);
        if (st + 1 < stacks) push_tri(p00, p10, p11, col);
        if (st > 0) push_tri(p00, p11, p01, col);
      }
    }
  };

  res.head_above_pivot_y = 0.325f;
  std::array<float, 3> head_clamped = head;
  if (p.clamp_roof) {
    const float roof_limit = p.roof_max_y - 0.19f;
    if (head_clamped[1] > roof_limit) head_clamped[1] = std::max(chest[1] + 0.18f, roof_limit);
  }

  const std::array<uint8_t, 4> jacket{56, 66, 82, 255};
  const std::array<uint8_t, 4> cuff{38, 44, 54, 255};
  const std::array<uint8_t, 4> wristband{224, 58, 92, 255};
  const std::array<uint8_t, 4> collar{168, 52, 68, 255};
  const std::array<uint8_t, 4> skin{184, 140, 114, 255};
  const std::array<uint8_t, 4> hair{48, 38, 32, 255};
  const std::array<uint8_t, 4> shades{22, 24, 30, 255};
  const std::array<uint8_t, 4> glove{38, 40, 46, 255};

  const auto l_wrist = vlerp(l_el, l_hand, 0.82f);
  const auto r_wrist = vlerp(r_el, r_hand, 0.82f);
  push_limb(l_el, l_wrist, 0.038f * p.hand_scale, jacket);
  push_limb(r_el, r_wrist, 0.038f * p.hand_scale, jacket);
  push_limb(l_wrist, l_hand, 0.032f * p.hand_scale, cuff);
  push_limb(r_wrist, r_hand, 0.032f * p.hand_scale, wristband);
  push_ellipsoid(l_hand, 0.042f * p.hand_scale, 0.036f * p.hand_scale, 0.046f * p.hand_scale, glove, 6, 10);
  push_ellipsoid(r_hand, 0.042f * p.hand_scale, 0.036f * p.hand_scale, 0.046f * p.hand_scale, glove, 6, 10);
  res.hands_count = count;

  push_limb(l_sh, l_el, 0.048f, jacket);
  push_limb(r_sh, r_el, 0.048f, jacket);
  push_box(chest, {0.19f, 0, 0}, {0, 0.21f, 0}, {0, 0, 0.105f}, jacket);
  push_box({chest[0], chest[1] + 0.16f, chest[2] - 0.02f}, {0.205f, 0, 0}, {0, 0.048f, 0}, {0, 0, 0.11f}, collar);
  push_box({head_clamped[0], head_clamped[1] - 0.11f, head_clamped[2]}, {0.052f, 0, 0}, {0, 0.060f, 0}, {0, 0, 0.052f}, skin);
  push_ellipsoid(head_clamped, 0.098f, 0.115f, 0.112f, skin, 8, 14);
  push_ellipsoid({head_clamped[0], head_clamped[1] + 0.042f, head_clamped[2] + 0.015f}, 0.104f, 0.088f, 0.114f, hair, 8, 14);
  push_ellipsoid({head_clamped[0], head_clamped[1] + 0.008f, head_clamped[2] - 0.084f}, 0.078f, 0.028f, 0.032f, shades, 5, 10);

  res.total_count = count;
  return res;
}

}  // namespace

bool Showroom::load_showcase_texture(const std::filesystem::path& game_root, int aircraft_kind, std::string& error) {
  if (guest_ || !r_.device_) return true;
  if (showcase_tex_kind_ == aircraft_kind && showcase_tex_set_ != VK_NULL_HANDLE) return true;

  bake::Texture tex;
  tex.format = bake::TextureFormat::kRgba8;
  bool decoded = false;
  if (!game_root.empty()) {
    const auto zip_path = cars::mods::resolve(game_root / "media" / "animatedobjects.zip");
    std::error_code ec;
    if (std::filesystem::exists(zip_path, ec)) {
      std::string aerr;
      if (auto arc = fh1::Archive::open(zip_path, &aerr)) {
        std::vector<const char*> candidates;
        switch (aircraft_kind) {
          case 1:  // Hot-Air Balloon
            candidates = {"Textures/FEST_AUTOSHOW_HotAirBalloon_DIFF.xds",
                          "Textures/FEST_AUTOSHOW_HotAirBalloon_2_DIFF.xds"};
            break;
          case 2:  // Stunt Jet
            candidates = {"Textures/ANIM_P-51_Mustang2_DIFF.xds", "Textures/ANIM_P-51_Mustang_DIFF.xds"};
            break;
          case 3:  // Biplane
            candidates = {"Textures/ANIM_BiplaneStearmanHRZN_Diff.xds",
                          "Textures/ANIM_BiplaneStearmanCropduster_Diff.xds"};
            break;
          case 4:  // Attack Helicopter
            candidates = {"Textures/GR_Heli_Blade_DIFF.xds", "Textures/ANIM_P-51_Mustang2_DIFF.xds"};
            break;
          default:  // P-51 Mustang
            candidates = {"Textures/ANIM_P-51_Mustang_DIFF.xds", "Textures/ANIM_P-51_Mustang2_DIFF.xds"};
            break;
        }
        for (const char* member : candidates) {
          const auto rd = arc->read(member);
          if (!rd.ok() || rd.data.empty()) continue;
          auto xr = fh1::decode_xds(rd.data, false);
          if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
              xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
            tex.levels.push_back({xr.levels[0].width, xr.levels[0].height, std::move(xr.levels[0].rgba)});
            decoded = true;
            break;
          }
        }
      }
    }
  }
  if (!decoded) {
    // Crisp procedural livery fallback (riveted metallic panels / festival gores / tactical steel).
    constexpr uint32_t kS = 128;
    std::vector<uint8_t> px(kS * kS * 4, 255);
    for (uint32_t y = 0; y < kS; ++y) {
      for (uint32_t x = 0; x < kS; ++x) {
        const size_t idx = (size_t(y) * kS + x) * 4;
        const bool seam = (x % 32 == 0) || (y % 32 == 0);
        const bool stripe = (y >= 54 && y <= 74);
        uint8_t r = 210, g = 216, b = 224;
        if (aircraft_kind == 1) {
          const int gore = int(x / 16) % 4;
          static constexpr uint8_t kPal[4][3] = {{238, 28, 108}, {252, 198, 26}, {252, 108, 22}, {244, 246, 252}};
          r = kPal[gore][0], g = kPal[gore][1], b = kPal[gore][2];
        } else if (aircraft_kind == 4) {
          r = stripe ? 215 : 58;
          g = stripe ? 64 : 66;
          b = stripe ? 38 : 72;
        } else if (aircraft_kind == 3) {
          r = stripe ? 245 : 206;
          g = stripe ? 236 : 36;
          b = stripe ? 210 : 34;
        } else if (aircraft_kind == 2) {
          r = stripe ? 242 : 226;
          g = stripe ? 20 : 230;
          b = stripe ? 98 : 238;
        } else {
          r = stripe ? 215 : 212;
          g = stripe ? 32 : 218;
          b = stripe ? 34 : 226;
        }
        const float shade = seam ? 0.78f : 1.0f;
        px[idx + 0] = uint8_t(float(r) * shade);
        px[idx + 1] = uint8_t(float(g) * shade);
        px[idx + 2] = uint8_t(float(b) * shade);
        px[idx + 3] = 255;
      }
    }
    tex.levels.push_back({kS, kS, std::move(px)});
  }

  r_.wait_idle();
  r_.destroy_image(showcase_tex_);
  if (!upload_texture(&tex, true, showcase_tex_, error)) return false;
  if (showcase_tex_set_ == VK_NULL_HANDLE) {
    if (!create_world_single_set(showcase_tex_.view, showcase_tex_set_, error)) return false;
  } else {
    VkDescriptorImageInfo di{sampler_, showcase_tex_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wd.dstSet = showcase_tex_set_;
    wd.dstBinding = 0;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &di;
    vkUpdateDescriptorSets(r_.device_, 1, &wd, 0, nullptr);
  }
  showcase_tex_kind_ = aircraft_kind;
  return true;
}

void Showroom::set_showcase_mesh(const std::vector<ShowcaseVertex>& tris) {
  showcase_count_ = 0;
  if (!vfx_ready_ || !showcase_buf_.ready() || tris.empty()) return;
  const uint32_t n = uint32_t(std::min(tris.size(), size_t(kMaxShowcaseVerts)));
  auto* dst = static_cast<WorldVertex*>(showcase_buf_.data());
  auto snorm8 = [](float x) -> int8_t {
    return int8_t(std::clamp(std::lround(double(x) * 127.0), -127l, 127l));
  };
  for (uint32_t i = 0; i < n; ++i) {
    const auto& v = tris[i];
    WorldVertex& o = dst[i];
    o = WorldVertex{};
    o.position[0] = float(double(v.pos[0]) - world_origin_[0]);
    o.position[1] = v.pos[1];
    o.position[2] = float(double(v.pos[2]) - world_origin_[1]);
    o.normal[0] = snorm8(v.normal[0]);
    o.normal[1] = snorm8(v.normal[1]);
    o.normal[2] = snorm8(v.normal[2]);
    o.normal[3] = 0;
    o.uv0[0] = v.uv[0];
    o.uv0[1] = v.uv[1];
    for (int c = 0; c < 4; ++c) o.colour[c] = v.rgba[c];
  }
  showcase_count_ = n;
  ++showcase_buf_.ver;
}

bool Showroom::load_driver_assets() {
  driver_assets_tried_ = true;
  driver_assets_loaded_ = false;
  if (guest_ || !r_.device_ || options_.game_root.empty()) return false;

  std::error_code ec;
  std::filesystem::path zip_path = cars::mods::resolve(options_.game_root / "media" / "cars" / "Driver.zip");
  if (!std::filesystem::exists(zip_path, ec)) {
    zip_path = cars::mods::resolve(options_.game_root / "media" / "cars" / "driver.zip");
  }
  if (!std::filesystem::exists(zip_path, ec)) return false;

  std::string aerr;
  auto arc = fh1::Archive::open(zip_path, &aerr);
  if (!arc) return false;

  std::string body_mem, hands_mem, band_mem, tex_mem;
  for (const auto& m : arc->members()) {
    const std::string ln = lower(m.name);
    const size_t slash = ln.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? ln : ln.substr(slash + 1);
    if (ends_with(base, ".skinbin")) {
      if (base == "driverhands01.skinbin" ||
          (hands_mem.empty() && base.find("driverhands") != std::string::npos)) {
        hands_mem = m.name;
      } else if (base == "driverwristband.skinbin" ||
                 (band_mem.empty() && base.find("wrist") != std::string::npos)) {
        band_mem = m.name;
      } else if (base == "driver01.skinbin") {
        body_mem = m.name;
      } else if (body_mem.empty() && base == "driver02.skinbin") {
        body_mem = m.name;
      } else if (body_mem.empty() && base.rfind("driver", 0) == 0 &&
                 base.find("temp") == std::string::npos && base.find("hands") == std::string::npos &&
                 base.find("wrist") == std::string::npos && base.find("left") == std::string::npos) {
        body_mem = m.name;
      }
    } else if (ends_with(base, ".xds")) {
      if (base == "driver01_d.xds") {
        tex_mem = m.name;
      } else if (tex_mem.empty() && base.find("driver") != std::string::npos &&
                 base.find("_d.") != std::string::npos) {
        tex_mem = m.name;
      }
    }
  }

  auto try_skin = [&](const std::string& mem, world::SpectatorMeshLod& dst) {
    if (mem.empty()) return;
    const auto rd = arc->read(mem);
    if (rd.ok() && !rd.data.empty()) {
      (void)world::parse_skinbin(rd.data, dst, 0);
    }
  };
  try_skin(body_mem, driver_body_mesh_);
  try_skin(hands_mem, driver_hands_mesh_);
  try_skin(band_mem, driver_wristband_mesh_);

  if (driver_body_mesh_.vertices.empty() || driver_body_mesh_.indices.size() < 3) return false;

  if (!tex_mem.empty() && driver_tex_set_ == VK_NULL_HANDLE) {
    const auto rd = arc->read(tex_mem);
    if (rd.ok() && !rd.data.empty()) {
      auto xr = fh1::decode_xds(rd.data, false);
      if (!xr.levels.empty() && xr.levels[0].width > 0 && xr.levels[0].height > 0 &&
          xr.levels[0].rgba.size() == size_t(xr.levels[0].width) * xr.levels[0].height * 4) {
        bake::Texture tex;
        tex.name = tex_mem;
        tex.format = bake::TextureFormat::kRgba8;
        tex.levels.push_back({xr.levels[0].width, xr.levels[0].height, std::move(xr.levels[0].rgba)});
        std::string uerr;
        if (upload_texture(&tex, true, driver_tex_, uerr)) {
          (void)create_world_single_set(driver_tex_.view, driver_tex_set_, uerr);
        }
      }
    }
  }

  driver_assets_loaded_ = true;
  return true;
}

void Showroom::update_driver_mesh(float wheel_rad) {
  driver_count_ = 0;
  driver_hands_count_ = 0;
  if (!driver_buf_.ready()) return;
  if (!driver_assets_tried_ && !guest_ && r_.device_ && !options_.game_root.empty()) {
    (void)load_driver_assets();
  }

  // Steering wheel centre, axis & grip radius in car model space (+X right, +Y up, -Z forward, +Z rearward),
  // incorporating CarAttribs.xml / CameraOverrides wheel offsets and DriverHandsOffset / DriverHandsScale.
  DriverPoseParams params{};
  if (cockpit_rigs_[0].valid) {
    params.pivot = cockpit_rigs_[0].pivot;
    params.axis = cockpit_rigs_[0].axis;
    params.rim_r = cockpit_rigs_[0].radius;
  }
  if (std::fabs(cockpit_wheel_rot_offset_rad_) > 1e-5f) {
    const float cr = std::cos(cockpit_wheel_rot_offset_rad_), sr = std::sin(cockpit_wheel_rot_offset_rad_);
    const float ny = cr * params.axis[1] - sr * params.axis[2];
    const float nz = sr * params.axis[1] + cr * params.axis[2];
    params.axis[1] = ny;
    params.axis[2] = nz;
  }
  params.pivot[0] += cockpit_wheel_pos_offset_[0] + driver_hands_offset_[0];
  params.pivot[1] += cockpit_wheel_pos_offset_[1] + driver_hands_offset_[1];
  params.pivot[2] -= cockpit_wheel_pos_offset_[2] + driver_hands_offset_[2];
  params.wheel_rad = wheel_rad;
  params.shift_blend = driver_shift_blend_;
  params.handbrake_blend = driver_handbrake_blend_;
  params.hand_scale = std::clamp(driver_hands_scale_, 0.65f, 1.45f);
  params.clamp_roof = have_bounds_;
  params.roof_max_y = bounds_max_[1];

  // Layout in `driver_buf_`:
  //   [0 .. driver_hands_count_)            : left/right forearms & hands (cockpit view draws only this range)
  //   [driver_hands_count_ .. driver_count_): torso, shoulders, legs & head (exterior player views draw all)
  const world::SpectatorMeshLod* skin = driver_assets_loaded_ ? &driver_body_mesh_ : nullptr;
  const DriverEmitResult res =
      emit_driver_geometry(skin, params, static_cast<WorldVertex*>(driver_buf_.data()), kMaxDriverVerts);
  driver_hands_count_ = res.hands_count;
  driver_count_ = res.total_count;
  if (driver_count_ > 0) ++driver_buf_.ver;
}

void Showroom::record_driver_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || !world_opaque_ || !world_white_set_) return;
  if (!driver_assets_tried_ && !guest_ && r_.device_ && !options_.game_root.empty()) {
    (void)load_driver_assets();
  }
  const uint32_t player_verts = cockpit_camera_active_ ? driver_hands_count_ : driver_count_;
  const bool draw_player = draw_car_ && player_verts > 0 && driver_buf_.ready();
  const bool has_guests = !instances_.empty();
  if (!draw_player && !has_guests) return;

  const bool use_tex = driver_assets_loaded_ && driver_tex_set_ != VK_NULL_HANDLE;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);
  const VkDescriptorSet sets[2] = {frame_set_, use_tex ? driver_tex_set_ : world_white_set_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);

  ShowroomPush pc{};
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3] * 0.78f;  // balanced cabin-shaded lighting visible through rear/side glass
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
  pc.params[0] = 0.0f;                             // kOpaque
  pc.params[1] = use_tex ? 1.0f : 65536.0f;        // kFlagTextured when Driver01_D.xds loaded, else kFlagVertexLit
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;

  if (draw_player) {
    VkDeviceSize at = dyn_sync(driver_buf_, VkDeviceSize(driver_count_) * sizeof(WorldVertex));
    vkCmdBindVertexBuffers(cmd, 0, 1, &driver_buf_.buf.buffer, &at);
    identity(pc.model);  // player driver vertices are in car model space
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, player_verts, 1, 0, 0);
  }

  // Render an independent neutral-pose seated driver inside nearby AI / festival / traffic cars,
  // positioned at each guest car's own LHD/RHD/center seat (never cloning the player's driver_buf_).
  if (!has_guests) return;
  GuestDriverCache* guest_cache = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_guest_driver_mutex);
    auto& ptr = g_guest_driver_caches[this];
    if (!ptr) {
      auto c = std::make_unique<GuestDriverCache>();
      const VkDeviceSize bytes = VkDeviceSize(kMaxDriverVerts) * sizeof(WorldVertex);
      if (r_.create_buffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, c->vb)) {
        if (vmaMapMemory(r_.allocator_, c->vb.allocation, &c->mapped) != VK_SUCCESS) {
          c->mapped = nullptr;
          r_.destroy_buffer(c->vb);
        }
      }
      if (c->mapped) {
        DriverPoseParams neutral{};
        neutral.pivot = {0.0f, 0.0f, 0.0f};
        neutral.wheel_rad = 0.0f;
        neutral.shift_blend = 0.0f;
        neutral.handbrake_blend = 0.0f;
        neutral.hand_scale = 1.0f;
        neutral.clamp_roof = false;
        const world::SpectatorMeshLod* skin = driver_assets_loaded_ ? &driver_body_mesh_ : nullptr;
        const DriverEmitResult gr =
            emit_driver_geometry(skin, neutral, static_cast<WorldVertex*>(c->mapped), kMaxDriverVerts);
        c->vert_count = gr.total_count;
        c->head_above_pivot_y = gr.head_above_pivot_y;
        c->built = true;
        ptr = std::move(c);
        Renderer* rptr = &r_;
        const Showroom* key = this;
        retired_.push_back({std::numeric_limits<uint64_t>::max(), [rptr, key]() {
                              std::unique_ptr<GuestDriverCache> doomed;
                              {
                                std::lock_guard<std::mutex> lk(g_guest_driver_mutex);
                                auto jt = g_guest_driver_caches.find(key);
                                if (jt != g_guest_driver_caches.end()) {
                                  doomed = std::move(jt->second);
                                  g_guest_driver_caches.erase(jt);
                                }
                              }
                              if (doomed && doomed->vb.allocation) {
                                if (doomed->mapped) vmaUnmapMemory(rptr->allocator_, doomed->vb.allocation);
                                rptr->destroy_buffer(doomed->vb);
                              }
                            }});
      }
    }
    guest_cache = ptr.get();
  }
  if (!guest_cache || !guest_cache->built || guest_cache->vert_count == 0) return;

  const VkDeviceSize zero_off = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &guest_cache->vb.buffer, &zero_off);

  auto resolve_guest_seat = [&](const Showroom* g) -> std::array<float, 3> {
    // Colorado US default: LHD seat (-X left, +Y up, -Z forward in car model space).
    std::array<float, 3> seat{-0.34f, 0.72f, 0.08f};
    if (!g) return seat;
    if (g->cockpit_rigs_[0].valid) {
      seat = g->cockpit_rigs_[0].pivot;
    } else {
      const auto& sw = g->car_.car.part_positions[9];
      if ((std::fabs(sw[0]) + std::fabs(sw[1]) + std::fabs(sw[2])) > 1e-4f) {
        // CarPartPositions[9] is in car frame (+Z forward); mirror Z for car model space (-Z forward).
        seat = {sw[0], sw[1], -sw[2]};
      } else if (g->have_bounds_) {
        seat[0] = -0.34f;
        seat[1] = std::clamp(g->bounds_max_[1] - guest_cache->head_above_pivot_y - 0.14f, 0.58f, 1.25f);
        seat[2] = 0.08f;
      }
    }
    if (g->have_bounds_) {
      const float head_top_y = seat[1] + guest_cache->head_above_pivot_y;
      const float roof_limit = g->bounds_max_[1] - 0.075f;
      if (head_top_y > roof_limit) {
        seat[1] -= std::min(head_top_y - roof_limit, 0.18f);
      }
    }
    return seat;
  };

  for (const auto& in : instances_) {
    if (!in.car || in.fade_alpha < 0.5f) continue;
    const float dx = in.model[12] - frame_.eye[0];
    const float dy = in.model[13] - frame_.eye[1];
    const float dz = in.model[14] - frame_.eye[2];
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) continue;
    if (dx * dx + dy * dy + dz * dz > 95.0f * 95.0f) continue;
    const auto seat = resolve_guest_seat(in.car);
    float seat_m[16];
    identity(seat_m);
    seat_m[12] = seat[0];
    seat_m[13] = seat[1];
    seat_m[14] = seat[2];
    multiply(in.model, seat_m, pc.model);
    vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, &pc);
    vkCmdDraw(cmd, guest_cache->vert_count, 1, 0, 0);
  }
}


void Showroom::record_showcase_draw(VkCommandBuffer cmd) {
  if (!vfx_ready_ || showcase_count_ == 0 || !world_opaque_) return;
  VkDescriptorSet mat_set = showcase_tex_set_ ? showcase_tex_set_ : world_white_set_;
  if (!mat_set) return;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_opaque_);
  vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
  r_.apply_vrs(cmd);
  const VkDescriptorSet sets[2] = {frame_set_, mat_set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline_layout_, 0, 2, sets, 0, nullptr);
  ShowroomPush pc{};
  std::memcpy(pc.model, world_transform_, sizeof pc.model);
  for (int i = 0; i < 3; ++i) {
    pc.uv0[i] = world_sun_[i];
    pc.uv1[i] = world_up_[i];
  }
  pc.uv0[3] = 2.0f;  // swap = none
  pc.uv1[3] = world_up_[3];
  pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;  // UV tiling 1x1
  pc.params[0] = 0.0f;  // kOpaque
  pc.params[1] = showcase_tex_set_ ? 1.0f : 65536.0f;  // kFlagTextured when showcase texture loaded
  pc.params[2] = 0.0f;
  pc.params[3] = 1.0f;
  vkCmdPushConstants(cmd, world_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof pc, &pc);
  VkDeviceSize at = dyn_sync(showcase_buf_, VkDeviceSize(showcase_count_) * sizeof(WorldVertex));
  vkCmdBindVertexBuffers(cmd, 0, 1, &showcase_buf_.buf.buffer, &at);
  vkCmdDraw(cmd, showcase_count_, 1, 0, 0);
}


}  // namespace carview
