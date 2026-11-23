#include "hub_scene.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "game_hud.h"
#include "hud_scene.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string lower(std::string s) {
  for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return s;
}

std::string credits_text(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  for (int k = int(s.size()) - 3; k > 0; k -= 3) s.insert(size_t(k), ",");
  return (v < 0 ? "-" : "") + s + " CR";
}

std::string col_text(sqlite3_stmt* st, int col) {
  const auto* p = sqlite3_column_text(st, col);
  return p ? reinterpret_cast<const char*>(p) : "";
}

const char* drive_label(int dt) {
  return dt == 1 ? "FWD" : dt == 3 ? "AWD" : "RWD";
}

// Column-major 4x4 matrix multiply: out = a * b.
void mul4(const float a[16], const float b[16], float out[16]) {
  float r[16];
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row)
      r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
                       a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
  std::copy_n(r, 16, out);
}

void build_look_at(const float eye[3], const float at[3], float view[16]) {
  float fx = at[0] - eye[0], fy = at[1] - eye[1], fz = at[2] - eye[2];
  const float fl = std::max(1e-6f, std::sqrt(fx * fx + fy * fy + fz * fz));
  fx /= fl;
  fy /= fl;
  fz /= fl;
  // Right = normalize(cross(f, up(0,1,0)))
  float rx = -fz, ry = 0.0f, rz = fx;
  const float rl = std::max(1e-6f, std::sqrt(rx * rx + rz * rz));
  rx /= rl;
  rz /= rl;
  // True up = cross(r, f)
  const float ux = ry * fz - rz * fy;
  const float uy = rz * fx - rx * fz;
  const float uz = rx * fy - ry * fx;

  view[0] = rx;   view[4] = ry;   view[8] = rz;    view[12] = -(rx * eye[0] + ry * eye[1] + rz * eye[2]);
  view[1] = ux;   view[5] = uy;   view[9] = uz;    view[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
  view[2] = -fx;  view[6] = -fy;  view[10] = -fz;  view[14] = (fx * eye[0] + fy * eye[1] + fz * eye[2]);
  view[3] = 0.0f; view[7] = 0.0f; view[11] = 0.0f; view[15] = 1.0f;
}

void build_perspective(float fovy_rad, float aspect, float znear, float zfar, bool reverse_z, float proj[16]) {
  std::fill_n(proj, 16, 0.0f);
  const float f = 1.0f / std::tan(0.5f * fovy_rad);
  proj[0] = f / std::max(0.1f, aspect);
  proj[5] = -f;  // Vulkan Y-down clip space
  proj[11] = -1.0f;
  if (reverse_z) {
    proj[10] = znear / (zfar - znear);
    proj[14] = (zfar * znear) / (zfar - znear);
  } else {
    proj[10] = -zfar / (zfar - znear);
    proj[14] = -(zfar * znear) / (zfar - znear);
  }
}

void draw_class_badge(ui::List& list, float x, float y, float h, int class_id, const std::string& cls_name,
                      int shown_pi) {
  const float bw = h * 1.45f;
  const float pw = h * 2.15f;
  const uint32_t c_bg = class_badge_color(class_id);
  list.rect(x, y, x + bw, y + h, c_bg, 3.0f);
  list.rect(x + bw, y, x + bw + pw, y + h, ui::rgba(242, 244, 248, 245), 3.0f);
  ui::TextStyle st_cls;
  st_cls.size = h * 0.62f;
  st_cls.color = ui::rgba(255, 255, 255);
  st_cls.align = ui::Align::kCenter;
  st_cls.font = ui::Font::kE;
  list.text(x + bw * 0.5f, y + h * 0.19f, cls_name, st_cls);

  ui::TextStyle st_pi = st_cls;
  st_pi.color = ui::rgba(18, 20, 24);
  st_pi.font = ui::Font::kB;
  list.text(x + bw + pw * 0.5f, y + h * 0.19f, std::to_string(shown_pi), st_pi);
}

void draw_stat_bar(ui::List& list, float x, float y, float w, float h, const char* label, double base_val,
                   double preview_val, bool show_diff) {
  const float label_w = 88.0f;
  const float val_w = 46.0f;
  const float bx = x + label_w;
  const float bw = std::max(40.0f, w - label_w - val_w - 8.0f);

  ui::TextStyle st;
  st.size = 13.0f;
  st.color = ui::rgba(205, 212, 224);
  st.font = ui::Font::kB;
  list.text(x, y + 1.0f, label, st);

  // Track background.
  const float bar_y = y + 4.0f;
  const float bar_h = std::max(7.0f, h - 8.0f);
  list.rect(bx, bar_y, bx + bw, bar_y + bar_h, ui::rgba(32, 36, 46, 220), 2.0f);

  const float u_base = float(std::clamp(base_val / 10.0, 0.0, 1.0));
  const float u_prev = float(std::clamp(preview_val / 10.0, 0.0, 1.0));

  if (!show_diff || std::fabs(preview_val - base_val) < 0.02) {
    list.rect(bx, bar_y, bx + bw * u_base, bar_y + bar_h, ui::rgba(235, 240, 248, 245), 2.0f);
  } else if (preview_val > base_val) {
    list.rect(bx, bar_y, bx + bw * u_base, bar_y + bar_h, ui::rgba(225, 232, 242, 240), 2.0f);
    list.rect(bx + bw * u_base, bar_y, bx + bw * u_prev, bar_y + bar_h, ui::rgba(60, 225, 115, 255), 2.0f);
  } else {
    list.rect(bx, bar_y, bx + bw * u_prev, bar_y + bar_h, ui::rgba(225, 232, 242, 240), 2.0f);
    list.rect(bx + bw * u_prev, bar_y, bx + bw * u_base, bar_y + bar_h, ui::rgba(240, 72, 62, 255), 2.0f);
  }

  char buf[24];
  std::snprintf(buf, sizeof buf, "%.1f", show_diff ? preview_val : base_val);
  ui::TextStyle stv = st;
  stv.align = ui::Align::kRight;
  if (show_diff && preview_val > base_val + 0.02) stv.color = ui::rgba(75, 235, 125);
  else if (show_diff && preview_val < base_val - 0.02) stv.color = ui::rgba(255, 92, 82);
  else stv.color = ui::rgba(255, 255, 255);
  list.text(x + w, y + 1.0f, buf, stv);
}

}  // namespace

uint32_t class_badge_color(int class_id) {
  switch (class_id) {
    case 0: return ui::rgba(110, 115, 125);  // F
    case 1: return ui::rgba(56, 152, 82);    // E
    case 2: return ui::rgba(46, 146, 212);   // D
    case 3: return ui::rgba(232, 196, 36);   // C
    case 4: return ui::rgba(234, 118, 28);   // B
    case 5: return ui::rgba(214, 42, 42);    // A
    case 6: return ui::rgba(146, 52, 192);   // S
    case 7: return ui::rgba(52, 74, 178);    // R3
    case 8: return ui::rgba(22, 152, 172);   // R2
    case 9: return ui::rgba(218, 46, 114);   // R1
    default: return ui::rgba(36, 190, 96);   // X
  }
}

const char* hub_room_name(HubRoom room) {
  switch (room) {
    case HubRoom::kAutoshow: return "HORIZON AUTOSHOW";
    case HubRoom::kWorkshop: return "DAK'S GARAGE";
    case HubRoom::kPaintShop: return "PAINT SHOP";
    case HubRoom::kGarage: return "MY GARAGE";
    case HubRoom::kBarnFind: return "BARN FIND RESTORATION";
    default: return "";
  }
}

HubCameraFocus focus_for_part_enum(int part_enum) {
  if (part_enum == 4 || part_enum == 5 || part_enum == 8 || part_enum == 10 || part_enum == 11 || part_enum == 38 ||
      part_enum == 39 || part_enum == 43) {
    return HubCameraFocus::kWheels;
  }
  if (part_enum == 9 || part_enum == 35 || part_enum == 18) return HubCameraFocus::kRear;
  if (part_enum == 34) return HubCameraFocus::kFront;
  if (part_enum == 36 || (part_enum >= 12 && part_enum <= 29)) return HubCameraFocus::kHood;
  if (part_enum == 37) return HubCameraFocus::kSide;
  if (part_enum == 40 || part_enum == 41) return HubCameraFocus::kInterior;
  return HubCameraFocus::kTurntable;
}

void HubCamera::set_focus(HubCameraFocus focus) {
  switch (focus) {
    case HubCameraFocus::kFront:
      target_yaw = 0.25f;
      target_pitch = 0.14f;
      target_dist = 3.9f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.50f;
      target_pos[2] = -0.8f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kRear:
      target_yaw = 2.65f;
      target_pitch = 0.20f;
      target_dist = 4.1f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.65f;
      target_pos[2] = 0.8f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kWheels:
      target_yaw = 1.28f;
      target_pitch = 0.10f;
      target_dist = 2.85f;
      target_pos[0] = 0.25f;
      target_pos[1] = 0.36f;
      target_pos[2] = -0.55f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kHood:
      target_yaw = 0.48f;
      target_pitch = 0.42f;
      target_dist = 3.8f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.65f;
      target_pos[2] = -0.75f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kRoof:
      target_yaw = 0.72f;
      target_pitch = 0.82f;
      target_dist = 4.4f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.70f;
      target_pos[2] = 0.0f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kSide:
      target_yaw = 1.57f;
      target_pitch = 0.12f;
      target_dist = 4.45f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.55f;
      target_pos[2] = 0.0f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kInterior:
      target_yaw = 0.95f;
      target_pitch = 0.32f;
      target_dist = 3.6f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.82f;
      target_pos[2] = 0.05f;
      auto_orbit = false;
      break;
    case HubCameraFocus::kTurntable:
    default:
      target_yaw = 0.62f;
      target_pitch = 0.17f;
      target_dist = 4.85f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.56f;
      target_pos[2] = 0.0f;
      auto_orbit = true;
      break;
  }
}

void HubCamera::nudge_orbit(float dyaw, float dpitch, float ddist) {
  auto_orbit = false;
  target_yaw += dyaw;
  target_pitch = std::clamp(target_pitch + dpitch, 0.04f, 1.18f);
  target_dist = std::clamp(target_dist + ddist, 2.3f, 8.5f);
}

void HubCamera::step(float dt) {
  if (auto_orbit) target_yaw += orbit_speed * dt;
  const float alpha = 1.0f - std::exp(-6.5f * std::max(0.0f, dt));
  yaw += (target_yaw - yaw) * alpha;
  pitch += (target_pitch - pitch) * alpha;
  distance += (target_dist - distance) * alpha;
  for (int i = 0; i < 3; ++i) target[i] += (target_pos[i] - target[i]) * alpha;
}

std::array<float, 3> hub_background_color(HubRoom room) {
  switch (room) {
    case HubRoom::kAutoshow: return {0.14f, 0.16f, 0.20f};   // crisp Colorado pavilion slate
    case HubRoom::kWorkshop: return {0.12f, 0.11f, 0.10f};   // Dak's warm industrial garage
    case HubRoom::kPaintShop: return {0.16f, 0.17f, 0.19f};  // neutral colour-true paint booth
    case HubRoom::kGarage: return {0.09f, 0.11f, 0.15f};     // Horizon festival bay
    case HubRoom::kBarnFind: return {0.13f, 0.10f, 0.07f};   // dusty golden barn light
    default: return {0.10f, 0.11f, 0.13f};
  }
}

WorldTriangles build_hub_stage(HubRoom room, float car_half_w, float car_half_l, float floor_y) {
  WorldTriangles wt;
  // Choose floor, plinth, accent ring, and shadow fan colours per Festival room.
  std::array<float, 3> floor_col = {0.18f, 0.19f, 0.22f};
  std::array<float, 3> plinth_col = {0.26f, 0.27f, 0.30f};
  std::array<float, 3> ring_col = {0.92f, 0.36f, 0.08f};  // Horizon signature coral-orange trim
  std::array<float, 3> wall_col = {0.11f, 0.12f, 0.15f};
  if (room == HubRoom::kWorkshop) {
    floor_col = {0.16f, 0.15f, 0.14f};
    plinth_col = {0.22f, 0.21f, 0.20f};
    ring_col = {0.95f, 0.62f, 0.12f};
    wall_col = {0.10f, 0.09f, 0.08f};
  } else if (room == HubRoom::kPaintShop) {
    floor_col = {0.22f, 0.23f, 0.25f};
    plinth_col = {0.30f, 0.31f, 0.33f};
    ring_col = {0.18f, 0.68f, 0.94f};
    wall_col = {0.14f, 0.15f, 0.17f};
  } else if (room == HubRoom::kBarnFind) {
    floor_col = {0.17f, 0.13f, 0.09f};
    plinth_col = {0.20f, 0.15f, 0.10f};
    ring_col = {0.82f, 0.56f, 0.22f};
    wall_col = {0.11f, 0.08f, 0.05f};
  }

  auto add_group = [&](std::array<float, 3> col) -> uint8_t {
    for (size_t i = 0; i < wt.colours.size(); ++i)
      if (wt.colours[i] == col) return uint8_t(i);
    wt.colours.push_back(col);
    return uint8_t(wt.colours.size() - 1);
  };
  auto push_tri_n = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c,
                        std::array<float, 3> na, std::array<float, 3> nb, std::array<float, 3> nc,
                        std::array<float, 3> col) {
    wt.positions.push_back(a);
    wt.positions.push_back(b);
    wt.positions.push_back(c);
    wt.normals.push_back(na);
    wt.normals.push_back(nb);
    wt.normals.push_back(nc);
    wt.group.push_back(add_group(col));
  };
  const std::array<float, 3> kUp{0.0f, 1.0f, 0.0f};
  auto push_tri = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c,
                      std::array<float, 3> col) {
    push_tri_n(a, b, c, kUp, kUp, kUp, col);
  };

  // 1. Outer studio floor disc (radius 18 m, 96 segments with 3 radial vignette bands).
  constexpr int kSegs = 96;
  const float y_outer = floor_y - 0.025f;
  const float floor_rings[4] = {3.25f, 7.2f, 12.6f, 18.0f};
  const float floor_shades[3] = {1.00f, 0.78f, 0.56f};
  for (int ring = 0; ring < 3; ++ring) {
    const std::array<float, 3> rcol{floor_col[0] * floor_shades[ring], floor_col[1] * floor_shades[ring],
                                    floor_col[2] * floor_shades[ring]};
    const float r0 = floor_rings[ring], r1 = floor_rings[ring + 1];
    for (int i = 0; i < kSegs; ++i) {
      const float a0 = 2.0f * kPi * float(i) / float(kSegs);
      const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
      std::array<float, 3> p00{r0 * std::cos(a0), y_outer, r0 * std::sin(a0)};
      std::array<float, 3> p01{r0 * std::cos(a1), y_outer, r0 * std::sin(a1)};
      std::array<float, 3> p10{r1 * std::cos(a0), y_outer, r1 * std::sin(a0)};
      std::array<float, 3> p11{r1 * std::cos(a1), y_outer, r1 * std::sin(a1)};
      push_tri(p00, p10, p11, rcol);
      push_tri(p00, p11, p01, rcol);
    }
  }

  // 2. Turntable plinth disc (radius 3.15 m) + glowing accent rim ring (3.15..3.25 m).
  const float y_plinth = floor_y - 0.008f;
  for (int i = 0; i < kSegs; ++i) {
    const float a0 = 2.0f * kPi * float(i) / float(kSegs);
    const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
    const float r_in = 0.0f, r_mid = 3.12f, r_out = 3.24f;
    std::array<float, 3> c0{r_in, y_plinth, 0.0f};
    std::array<float, 3> m0{r_mid * std::cos(a0), y_plinth, r_mid * std::sin(a0)};
    std::array<float, 3> m1{r_mid * std::cos(a1), y_plinth, r_mid * std::sin(a1)};
    std::array<float, 3> o0{r_out * std::cos(a0), y_plinth + 0.002f, r_out * std::sin(a0)};
    std::array<float, 3> o1{r_out * std::cos(a1), y_plinth + 0.002f, r_out * std::sin(a1)};
    push_tri(c0, m0, m1, plinth_col);
    push_tri(m0, o0, o1, ring_col);
    push_tri(m0, o1, m1, ring_col);
  }

  // 3. Elliptical soft drop-shadow floor fan directly under the car's footprint
  //    (3 concentric rings fading from deep ambient occlusion under the chassis
  //    to the plinth tone outside the tyres).
  const float sx = std::max(0.75f, car_half_w * 1.06f);
  const float sz = std::max(1.65f, car_half_l * 1.04f);
  const float scales[4] = {0.0f, 0.72f, 1.00f, 1.24f};
  const float darks[3] = {0.08f, 0.26f, 0.62f};
  const float y_sh = floor_y - 0.003f;
  for (int ring = 0; ring < 3; ++ring) {
    std::array<float, 3> col = {plinth_col[0] * darks[ring], plinth_col[1] * darks[ring],
                                plinth_col[2] * darks[ring]};
    const float ra = scales[ring];
    const float rb = scales[ring + 1];
    const float y_ring = y_sh + 0.001f * float(2 - ring);
    for (int i = 0; i < kSegs; ++i) {
      const float a0 = 2.0f * kPi * float(i) / float(kSegs);
      const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
      if (ring == 0) {
        std::array<float, 3> c0{0.0f, y_ring, 0.0f};
        std::array<float, 3> p0{sx * rb * std::cos(a0), y_ring, sz * rb * std::sin(a0)};
        std::array<float, 3> p1{sx * rb * std::cos(a1), y_ring, sz * rb * std::sin(a1)};
        push_tri(c0, p0, p1, col);
      } else {
        std::array<float, 3> q00{sx * ra * std::cos(a0), y_ring, sz * ra * std::sin(a0)};
        std::array<float, 3> q01{sx * ra * std::cos(a1), y_ring, sz * ra * std::sin(a1)};
        std::array<float, 3> q10{sx * rb * std::cos(a0), y_ring, sz * rb * std::sin(a0)};
        std::array<float, 3> q11{sx * rb * std::cos(a1), y_ring, sz * rb * std::sin(a1)};
        push_tri(q00, q10, q11, col);
        push_tri(q00, q11, q01, col);
      }
    }
  }

  // 4. Smooth 96-segment studio cyclorama wall (radius 17.5 m, height 8.5 m) with inward smooth
  //    vertex normals and subtle studio light-box gradient bands + horizon accent trim.
  const float wall_y[5] = {y_outer, y_outer + 0.8f, y_outer + 3.2f, y_outer + 5.8f, y_outer + 8.5f};
  const float wall_shades[4] = {0.72f, 1.08f, 0.92f, 0.62f};
  for (int band = 0; band < 4; ++band) {
    const std::array<float, 3> bcol{wall_col[0] * wall_shades[band], wall_col[1] * wall_shades[band],
                                    wall_col[2] * wall_shades[band]};
    const float yb = wall_y[band], yt = wall_y[band + 1];
    for (int i = 0; i < kSegs; ++i) {
      const float a0 = 2.0f * kPi * float(i) / float(kSegs);
      const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
      const float c0 = std::cos(a0), s0 = std::sin(a0);
      const float c1 = std::cos(a1), s1 = std::sin(a1);
      const float r = 17.5f;
      const std::array<float, 3> n0{-c0, 0.08f, -s0};
      const std::array<float, 3> n1{-c1, 0.08f, -s1};
      std::array<float, 3> b0{r * c0, yb, r * s0};
      std::array<float, 3> b1{r * c1, yb, r * s1};
      std::array<float, 3> t0{r * c0, yt, r * s0};
      std::array<float, 3> t1{r * c1, yt, r * s1};
      push_tri_n(b0, t0, t1, n0, n0, n1, bcol);
      push_tri_n(b0, t1, b1, n0, n1, n1, bcol);
    }
  }
  // Subtle Horizon studio light-strip band around upper studio truss.
  {
    const std::array<float, 3> strip_col{wall_col[0] * 1.45f + ring_col[0] * 0.14f,
                                         wall_col[1] * 1.45f + ring_col[1] * 0.14f,
                                         wall_col[2] * 1.45f + ring_col[2] * 0.14f};
    const float r = 17.38f;
    const float yb = y_outer + 3.15f, yt = y_outer + 3.30f;
    for (int i = 0; i < kSegs; ++i) {
      const float a0 = 2.0f * kPi * float(i) / float(kSegs);
      const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
      const float c0 = std::cos(a0), s0 = std::sin(a0);
      const float c1 = std::cos(a1), s1 = std::sin(a1);
      const std::array<float, 3> n0{-c0, 0.0f, -s0};
      const std::array<float, 3> n1{-c1, 0.0f, -s1};
      push_tri_n({r * c0, yb, r * s0}, {r * c0, yt, r * s0}, {r * c1, yt, r * s1}, n0, n0, n1, strip_col);
      push_tri_n({r * c0, yb, r * s0}, {r * c1, yt, r * s1}, {r * c1, yb, r * s1}, n0, n1, n1, strip_col);
    }
  }

  return wt;
}

void build_hub_frame(HubRoom room, const HubCamera& cam, uint32_t width, uint32_t height, ShowroomFrame& out,
                     bool reverse_z) {
  out = ShowroomFrame{};
  const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
  const float eye[3] = {
      cam.target[0] + cam.distance * cp * std::sin(cam.yaw),
      cam.target[1] + cam.distance * sp,
      cam.target[2] - cam.distance * cp * std::cos(cam.yaw),
  };
  float view[16], proj[16], vp[16];
  build_look_at(eye, cam.target, view);
  const float aspect = height > 0 ? float(width) / float(height) : (16.0f / 9.0f);
  build_perspective(38.0f * (kPi / 180.0f), aspect, 0.1f, 120.0f, reverse_z, proj);
  mul4(proj, view, vp);

  std::copy_n(vp, 16, out.view_projection);
  std::copy_n(vp, 16, out.transparent_view_projection);
  out.eye[0] = eye[0];
  out.eye[1] = eye[1];
  out.eye[2] = eye[2];
  out.eye[3] = 1.0f;

  // Studio lighting (drive.w = 0 selects staticcarcubemap.xpr environment reflections!).
  out.drive[0] = 0.0f;
  out.drive[1] = 1.0f;
  out.drive[2] = 0.0f;
  out.drive[3] = 0.0f;

  out.key_light[0] = 0.45f;
  out.key_light[1] = 0.78f;
  out.key_light[2] = -0.42f;
  out.key_light[3] = 0.65f;  // SHOWROOM_LIGHTING.md: 0.6525 key with HDR staticcarcubemap.xpr

  out.fill_light[0] = -0.60f;
  out.fill_light[1] = 0.45f;
  out.fill_light[2] = 0.66f;
  out.fill_light[3] = 0.12f;

  out.options[0] = 0.46f;  // exposure: holds white paint at ~190-215/255 with crisp HDR studio strips
  out.options[1] = 0.90f;  // studio cubemap reflection intensity (staticcarcubemap.xpr 80-97 HDR)
  out.options[2] = 0.0f;   // livery debug mask off
  out.options[3] = 1.0f;   // full Metallic.xml two-layer metallic response

  out.tint[0] = out.tint[1] = out.tint[2] = out.tint[3] = 1.0f;
  out.reflect_tint[0] = out.reflect_tint[1] = out.reflect_tint[2] = out.reflect_tint[3] = 1.0f;

  if (room == HubRoom::kWorkshop) {
    out.tint[0] = 1.04f;
    out.tint[1] = 0.98f;
    out.tint[2] = 0.91f;
    out.options[0] = 0.44f;
  } else if (room == HubRoom::kPaintShop) {
    out.options[0] = 0.50f;
    out.options[1] = 0.95f;
  } else if (room == HubRoom::kBarnFind) {
    out.tint[0] = 1.10f;
    out.tint[1] = 0.94f;
    out.tint[2] = 0.78f;
    out.options[0] = 0.42f;
  }
}

// --- GarageBrowser ---

const char* garage_sort_name(GarageSort s) {
  switch (s) {
    case GarageSort::kClass: return "CLASS";
    case GarageSort::kMake: return "MAKE";
    case GarageSort::kValue: return "VALUE";
    case GarageSort::kDrivetrain: return "DRIVETRAIN";
    case GarageSort::kPi: return "PI";
    default: return "CLASS";
  }
}

void GarageBrowser::refresh(const garage::Garage& g, const cars::PartsDb* db,
                            const std::vector<garage::ClassBand>& bands) {
  all_.clear();
  std::map<int64_t, std::string> make_names;
  struct Meta {
    std::string media, display, make;
    int year = 0, drive = 2;
    int64_t base_cost = 0;
  };
  std::map<int64_t, Meta> metas;
  if (db && db->is_open()) {
    db->each("SELECT ID, IconPathBase, ManufacturerCode FROM List_CarMake", {},
             [&](sqlite3_stmt* st) {
               std::string n = col_text(st, 1);
               if (n.empty()) n = col_text(st, 2);
               make_names[sqlite3_column_int64(st, 0)] = n;
             },
             nullptr);
    db->each("SELECT Id, MediaName, DisplayName, MakeID, Year, DriveTypeID, BaseCost FROM Data_Car", {},
             [&](sqlite3_stmt* st) {
               Meta m;
               const int64_t id = sqlite3_column_int64(st, 0);
               m.media = col_text(st, 1);
               m.display = col_text(st, 2);
               const int64_t mid = sqlite3_column_int64(st, 3);
               if (auto it = make_names.find(mid); it != make_names.end()) m.make = it->second;
               m.year = sqlite3_column_int(st, 4);
               m.drive = sqlite3_column_int(st, 5);
               m.base_cost = sqlite3_column_int64(st, 6);
               metas[id] = std::move(m);
             },
             nullptr);
  }

  for (const auto& c : g.cars) {
    GarageEntry e;
    e.garage_id = c.id;
    e.car_id = c.car_id;
    e.raw_pi = c.performance_index;
    e.class_id = bands.empty() ? c.class_id : garage::class_of(c.performance_index, bands);
    e.shown_pi = bands.empty() ? int(std::ceil(c.performance_index * 999.0))
                               : garage::shown_index(c.performance_index, bands);
    e.colour_seq = c.colour;
    e.is_current = (c.id == g.selected);
    int64_t base_cost = 0;
    if (auto it = metas.find(c.car_id); it != metas.end()) {
      e.media_name = it->second.media;
      e.display_name = it->second.display.empty() ? car_name(it->second.media) : it->second.display;
      e.make_name = it->second.make.empty() ? "Other" : it->second.make;
      e.year = it->second.year;
      e.drive_type_id = it->second.drive;
      base_cost = it->second.base_cost;
    } else {
      e.display_name = "Car #" + std::to_string(c.car_id);
      e.make_name = "Other";
    }
    e.value_cr = base_cost + c.parts_value;
    e.sell_cr = garage::sell_car_value(base_cost, c.parts_value);
    all_.push_back(std::move(e));
  }
  apply_filter_and_sort();
}

void GarageBrowser::apply_filter_and_sort() {
  visible_.clear();
  for (const auto& e : all_) {
    if (class_filter_ >= 0 && e.class_id != class_filter_) continue;
    visible_.push_back(e);
  }
  std::sort(visible_.begin(), visible_.end(), [&](const GarageEntry& a, const GarageEntry& b) {
    switch (sort_) {
      case GarageSort::kMake:
        if (lower(a.make_name) != lower(b.make_name)) return lower(a.make_name) < lower(b.make_name);
        return a.raw_pi < b.raw_pi;
      case GarageSort::kValue:
        if (a.value_cr != b.value_cr) return a.value_cr > b.value_cr;
        return a.raw_pi > b.raw_pi;
      case GarageSort::kDrivetrain:
        if (a.drive_type_id != b.drive_type_id) return a.drive_type_id < b.drive_type_id;
        return a.raw_pi < b.raw_pi;
      case GarageSort::kPi:
        if (a.raw_pi != b.raw_pi) return a.raw_pi > b.raw_pi;
        return a.garage_id < b.garage_id;
      case GarageSort::kClass:
      default:
        if (a.class_id != b.class_id) return a.class_id < b.class_id;
        return a.raw_pi < b.raw_pi;
    }
  });
  selected_ = visible_.empty() ? 0 : std::min(selected_, visible_.size() - 1);
}

void GarageBrowser::move(int by) {
  if (visible_.empty()) return;
  const int n = int(visible_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
}

void GarageBrowser::cycle_sort(int by) {
  const int n = int(GarageSort::kCount);
  sort_ = GarageSort(((int(sort_) + by) % n + n) % n);
  apply_filter_and_sort();
}

void GarageBrowser::cycle_class_filter(int by) {
  // Cycle: -1 (All), 1 (E), 2 (D), ..., 9 (R1).
  static constexpr int kCycle[] = {-1, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  constexpr int n = int(sizeof(kCycle) / sizeof(kCycle[0]));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (kCycle[i] == class_filter_) idx = i;
  idx = ((idx + by) % n + n) % n;
  class_filter_ = kCycle[idx];
  selected_ = 0;
  apply_filter_and_sort();
}

bool GarageBrowser::sell_selected(garage::Garage& g, const cars::PartsDb* db,
                                  const std::vector<garage::ClassBand>& bands, int64_t& credits,
                                  std::string& message) {
  const GarageEntry* e = selected_entry();
  if (!e) {
    message = "no car selected";
    return false;
  }
  const int64_t base_cost = std::max<int64_t>(0, e->value_cr - (g.find(e->garage_id) ? g.find(e->garage_id)->parts_value : 0));
  const garage::SellCarResult sr = garage::sell_car(g, e->garage_id, base_cost, credits);
  if (!sr.ok) {
    message = sr.reason;
    return false;
  }
  message = "sold " + e->display_name + " for +" + credits_text(sr.payout);
  refresh(g, db, bands);
  return true;
}

// --- Authentic FH1 UI4 +3.0 deg .bgf-Driven 3D Festival Hub Overlays ---

namespace {

// Draws a BIGPANE01 carousel card (DIRTMASKS/BIGPANE01 + BGNOISE01 + #F90064 focus frame).
void draw_bigpane_card(ui::List& list, float x0, float y0, float x1, float y1, float s, bool active,
                       uint32_t focus_col = ui4::kPink) {
  list.rect(x0 + 3.0f * s, y0 + 4.0f * s, x1 + 3.0f * s, y1 + 4.0f * s, ui::rgba(0, 0, 0, 96));
  const uint32_t bg = active ? ui::rgba(36, 16, 30, 240) : ui::rgba(20, 22, 26, 224);
  std::array<float, 4> pane_uv{}, noise_uv{};
  if (ui4::texture("DIRTMASKS/BIGPANE01", pane_uv)) {
    list.image(x0, y0, x1, y1, pane_uv, bg);
    if (ui4::texture("DIRTMASKS/BGNOISE01", noise_uv)) {
      list.image_masked(x0, y0, x1, y1, noise_uv, pane_uv, ui::rgba(255, 255, 255, 24), {1.0f, 1.0f, 0.0f, 0.0f},
                        ui::kMaskRepeat);
    }
  } else {
    ui4::panel(list, x0, y0, x1, y1, s, bg, 0, 0, true);
  }
  if (active) {
    list.rect(x0, y0, x1, y0 + 4.0f * s, focus_col);
    list.frame(x0, y0, x1, y1, focus_col, 2.2f * s);
  } else {
    list.rect(x0, y0, x1, y0 + 1.5f * s, ui::rgba(255, 255, 255, 36));
  }
}

// Extracts +3.0 deg RotationHelper angle from a loaded scene (falling back to ui4::kTiltRad).
float scene_tilt_rad(const char* scene_name, const char* root_node = "RotationHelper") {
  if (const fh1::UiScene* sc_ptr = ui4::scene(scene_name)) {
    SceneDraw sc(sc_ptr, true);
    if (const int n = sc.find(root_node); n >= 0) {
      const float a = -sc.angle(n);
      if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) return a;
    }
  }
  return ui4::kTiltRad;
}

// Top-left PlayerBar (108/115/527 authentic top-left @(387,41) credits + optional discount/tag).
void draw_hub_player_bar(ui::List& list, float s, int64_t credits, std::string_view badge_label = {},
                         int discount_percent = 0) {
  const float x0 = 56.0f * s, y0 = 26.0f * s;
  if (!badge_label.empty()) {
    ui4::pill(list, x0, y0, x0 + 236.0f * s, y0 + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
    ui::TextStyle bs = ui4::style(ui::Font::kE, 13.0f * s, ui4::kWhite);
    ui4::fit_text(badge_label, bs, 214.0f * s, 9.0f * s);
    list.text(x0 + 14.0f * s, y0 + 7.0f * s, badge_label, bs);
  }
  const float cr_x0 = badge_label.empty() ? x0 : (x0 + 244.0f * s);
  ui4::offwhite_strip(list, cr_x0, y0, cr_x0 + 178.0f * s, y0 + 28.0f * s, s, true);
  ui::TextStyle crs = ui4::style(ui::Font::kB, 16.0f * s, ui4::kInk, ui::Align::kRight, true);
  const std::string cstr = ui4::format_credits(credits);
  ui4::fit_text(cstr, crs, 160.0f * s, 9.5f * s);
  list.text(cr_x0 + 166.0f * s, y0 + 6.0f * s, cstr, crs);

  if (discount_percent > 0) {
    char db[32];
    std::snprintf(db, sizeof db, "%d%% OFF", discount_percent);
    ui4::brush(list, cr_x0 + 186.0f * s, y0, cr_x0 + 292.0f * s, y0 + 28.0f * s, ui4::kPink);
    list.text(cr_x0 + 239.0f * s, y0 + 7.0f * s, db,
              ui4::style(ui::Font::kE, 13.0f * s, ui4::kWhite, ui::Align::kCenter));
  }
}

}  // namespace

void draw_autoshow_hud(ui::List& list, float w, float h, const Autoshow& autoshow,
                       const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.20f);

  const AutoshowStep step = autoshow.step();
  const char* scene_name = (step == AutoshowStep::kMakeSelect)  ? "115_C_BUY_MFRSELECT"
                           : (step == AutoshowStep::kColorSelect) ? "114_C_BUY_BUYCAR_COLOR"
                                                                  : "108_C_BUY_CARSELECT";
  const float tilt_rad = scene_tilt_rad(scene_name, "RotationHelper");

  // Top-left PlayerBar (authentic 115/108/114 PlayerBar@(387,41), leaving center 3D car open!).
  const auto& makes = autoshow.makes();
  const std::string active_make =
      makes.empty() ? std::string("AUTOSHOW") : makes[std::min(autoshow.selected_make_index(), makes.size() - 1)].name;
  draw_hub_player_bar(list, s, credits,
                      autoshow.tutorial_mode() ? "FIRST CAR SELECTION"
                      : (step == AutoshowStep::kMakeSelect) ? "HORIZON AUTOSHOW"
                                                            : ("AUTOSHOW  //  " + active_make));

  // Entire Autoshow UI sits inside RotationHelper@(640,360) tilted +3.0 deg!
  {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);

    if (step == AutoshowStep::kMakeSelect) {
      // ---- 115_C_BUY_MFRSELECT: Lower-left TitleTapeText@(91,435) + Bottom Manufacturer Logo Carousel ----
      ui4::title_tape(list, s, 88.0f * s, 424.0f * s, "AUTOSHOW  //  MANUFACTURERS", ui4::kBrushBlack, 36.0f, 560.0f);

      if (!makes.empty()) {
        const float strip_y = 492.0f * s;
        const float card_h = 128.0f * s;
        const int n = int(makes.size());
        const int sel = int(autoshow.selected_make_index());
        const int count = std::min(n, 6);
        const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
        const float left_x = 68.0f * s, right_x = w - 68.0f * s, gap = 12.0f * s;
        const float cw = (right_x - left_x - gap * float(std::max(0, count - 1))) / float(std::max(1, count));

        float bx = left_x;
        for (int i = first; i < first + count; ++i) {
          const auto& mk = makes[size_t(i)];
          const bool act = (i == sel);
          const float lift = 0.0f;
          list.add_hit(bx, strip_y - lift, bx + cw, strip_y + card_h - lift, ui::kHitRow, i);
          draw_bigpane_card(list, bx, strip_y - lift, bx + cw, strip_y + card_h - lift, s, act);

          const float lh = 64.0f * s;
          const float lw = std::min(cw - 24.0f * s, lh * 1.45f);
          const float lx0 = bx + 0.5f * (cw - lw);
          const float ly0 = strip_y + 12.0f * s - lift;
          if (!ui4::manufacturer_logo(list, lx0, ly0, lx0 + lw, ly0 + lh, mk.name,
                                      act ? ui4::kWhite : ui::rgba(220, 224, 232, 225))) {
            ui::TextStyle ms = ui4::style(ui::Font::kE, 18.0f * s, act ? ui4::kWhite : ui4::kDim, ui::Align::kCenter);
            ui4::fit_text(mk.name, ms, cw - 16.0f * s, 10.0f * s);
            list.text(bx + 0.5f * cw, strip_y + 36.0f * s - lift, mk.name, ms);
          }

          ui::TextStyle nm = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kCenter);
          ui4::fit_text(mk.name, nm, cw - 14.0f * s, 8.5f * s);
          list.text(bx + 0.5f * cw, strip_y + 84.0f * s - lift, mk.name, nm);

          list.text(bx + 0.5f * cw, strip_y + 104.0f * s - lift,
                    std::to_string(mk.car_count) + (mk.car_count == 1 ? " CAR" : " CARS"),
                    ui4::style(ui::Font::kB, 11.0f * s, act ? ui4::kYellow : ui4::kDim, ui::Align::kCenter));
          bx += cw + gap;
        }

        // TEXT_CURSOR @(652, 608.7)
        char cur[32];
        std::snprintf(cur, sizeof cur, "%d / %d", sel + 1, n);
        ui4::pill(list, 0.5f * w - 58.0f * s, strip_y + card_h + 6.0f * s, 0.5f * w + 58.0f * s,
                  strip_y + card_h + 28.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
        list.text(0.5f * w, strip_y + card_h + 11.0f * s, cur,
                  ui4::style(ui::Font::kD, 14.0f * s, ui4::kWhite, ui::Align::kCenter));
      }
    } else if (step == AutoshowStep::kColorSelect) {
      // ---- 114_C_BUY_BUYCAR_COLOR: Bottom +3.0 deg Factory Color Swatch Strip (y=567..641) ----
      const AutoshowCar* sel_car = autoshow.selected_car();
      std::string cname = sel_car ? car_name(sel_car->dealer.media_name) : std::string("AUTOSHOW");
      if (sel_car && cname.empty()) cname = sel_car->display_name;
      ui4::title_tape(list, s, 88.0f * s, 488.0f * s, "FACTORY COLOR  //  " + cname, ui4::kBrushBlack, 30.0f, 760.0f);

      if (sel_car) {
        const float sy0 = 562.0f * s, sy1 = 626.0f * s;
        const size_t nsw = std::min<size_t>(sel_car->swatches.size(), 14);
        const float sw_w = 46.0f * s, sw_h = 46.0f * s, sw_gap = 6.0f * s;
        float sx = 86.0f * s;

        std::array<float, 4> chip_uv{}, met_uv{}, focus_uv{}, grad_uv{};
        const bool has_chip = ui4::texture("LIVERY/CAR_PAINT_GRID_CHIP", chip_uv);
        const bool has_met = ui4::texture("LIVERY/CAR_PAINT_GRID_METALLIC", met_uv);
        const bool has_focus = ui4::texture("LIVERY/CAR_PAINT_GRID_FOCUS", focus_uv);
        const bool has_grad = ui4::texture("LIVERY/CAR_PAINT_LIST_GRADIENT", grad_uv);
        const uint32_t kFocusPink = ui::rgba(0xCF, 0x21, 0x69, 255);

        ui4::pill(list, sx - 12.0f * s, sy0 - 8.0f * s, sx + float(std::max<size_t>(1, nsw)) * (sw_w + sw_gap) + 10.0f * s,
                  sy1 + 6.0f * s, ui4::kPillGrey, ui4::PillKind::kPillBig);
        if (has_grad) {
          list.image(sx - 12.0f * s, sy0 - 8.0f * s, sx + float(std::max<size_t>(1, nsw)) * (sw_w + sw_gap) + 10.0f * s,
                     sy1 + 6.0f * s, grad_uv, ui::rgba(255, 255, 255, 42));
        }

        for (size_t i = 0; i < nsw; ++i) {
          const auto& sw = sel_car->swatches[i];
          const uint32_t sw_col =
              ui::rgba(uint8_t((sw.rgb >> 16) & 0xFF), uint8_t((sw.rgb >> 8) & 0xFF), uint8_t(sw.rgb & 0xFF), 255);
          const bool active = (i == autoshow.selected_colour_index());
          const float y_c = sy0 + 6.0f * s - (active ? 4.0f * s : 0.0f);
          list.rect(sx, y_c, sx + sw_w, y_c + sw_h, sw_col);
          if (sw.metallic && has_met) list.image(sx, y_c, sx + sw_w, y_c + sw_h, met_uv, ui::rgba(255, 255, 255, 96));
          else if (has_chip) list.image(sx, y_c, sx + sw_w, y_c + sw_h, chip_uv, ui::rgba(255, 255, 255, 68));
          if (active) {
            if (has_focus) {
              list.image(sx - 4.0f * s, y_c - 4.0f * s, sx + sw_w + 4.0f * s, y_c + sw_h + 4.0f * s, focus_uv,
                         kFocusPink);
            }
            list.frame(sx - 3.0f * s, y_c - 3.0f * s, sx + sw_w + 3.0f * s, y_c + sw_h + 3.0f * s, kFocusPink, 3.0f * s);
            list.frame(sx - 1.0f * s, y_c - 1.0f * s, sx + sw_w + 1.0f * s, y_c + sw_h + 1.0f * s, ui4::kWhite, 1.5f * s);
          } else {
            list.frame(sx, y_c, sx + sw_w, y_c + sw_h, ui::rgba(0, 0, 0, 125), 1.0f * s);
          }
          sx += sw_w + sw_gap;
        }

        // Right-aligned Price & Balance (114 x=836..1168, y=566..637)
        const float px0 = w - 344.0f * s, px1 = w - 68.0f * s;
        const bool affordable = credits >= sel_car->dealer.base_cost;
        ui4::pill(list, px0, sy0 - 4.0f * s, px1, sy0 + 26.0f * s, ui4::kPink, ui4::PillKind::kPill26);
        list.text(px0 + 14.0f * s, sy0 + 3.0f * s, "PRICE", ui4::style(ui::Font::kD, 14.0f * s, ui4::kWhite));
        list.text(px1 - 14.0f * s, sy0 + 3.0f * s, ui4::format_credits(sel_car->dealer.base_cost),
                  ui4::style(ui::Font::kE, 15.0f * s, affordable ? ui4::kWhite : ui4::kYellow, ui::Align::kRight, true));

        ui4::offwhite_strip(list, px0, sy0 + 30.0f * s, px1, sy0 + 58.0f * s, s, true);
        list.text(px0 + 14.0f * s, sy0 + 36.0f * s, "BALANCE", ui4::style(ui::Font::kB, 13.0f * s, ui4::kInk));
        list.text(px1 - 14.0f * s, sy0 + 36.0f * s, ui4::format_credits(credits),
                  ui4::style(ui::Font::kB, 15.0f * s, ui4::kInk, ui::Align::kRight, true));
      }
    } else {
      // ---- 108_C_BUY_CARSELECT: Tilted Right Stat Card + Lower-Left TitleTape + Bottom BIGPANE01 Carousel ----
      const AutoshowCar* sel_car = autoshow.selected_car();

      // Right-hand tilted +3.0 deg Stat Card (108_C_BUY_CARSELECT x=815..1220, y=38..434; NO giant flat wall!)
      const float cx0 = w - 432.0f * s, cx1 = w - 58.0f * s;
      const float cw_card = cx1 - cx0;
      float cy = 40.0f * s;

      if (!sel_car) {
        ui4::panel01(list, cx0, cy, cx1, cy + 96.0f * s, s, ui4::kCharcoalDark, ui4::kAmber);
        list.text(0.5f * (cx0 + cx1), cy + 38.0f * s,
                  autoshow.empty_reason().empty() ? "NO CARS AVAILABLE" : autoshow.empty_reason(),
                  ui4::style(ui::Font::kE, 18.0f * s, ui4::kAmber, ui::Align::kCenter));
      } else {
        const int cls = bands.empty() ? int(sel_car->dealer.class_id)
                                      : garage::class_of(sel_car->dealer.performance_index, bands);
        const std::string cls_name = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
        const int shown_pi = bands.empty() ? int(std::ceil(sel_car->dealer.performance_index * 999.0))
                                           : garage::shown_index(sel_car->dealer.performance_index, bands);

        // TEXT_CAR_NAME pill + TEXT_MANUFACTURER + PICLASS badge (108 top block y=40..124)
        std::string cname = car_name(sel_car->dealer.media_name);
        if (cname.empty()) cname = sel_car->display_name;
        ui4::brush(list, cx0 - 8.0f * s, cy, cx1 + 4.0f * s, cy + 34.0f * s, ui4::kPink);
        ui::TextStyle nm = ui4::style(ui::Font::kD, 17.5f * s, ui4::kWhite);
        ui4::fit_text(cname, nm, cw_card - 26.0f * s, 11.0f * s);
        list.text(cx0 + 12.0f * s, cy + 8.0f * s + 0.5f * (17.5f * s - nm.size), cname, nm);
        cy += 36.0f * s;

        ui4::pill(list, cx0, cy, cx1, cy + 32.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02, false, true);
        float mfx = cx0 + 12.0f * s;
        if (ui4::manufacturer_logo(list, cx0 + 10.0f * s, cy + 4.0f * s, cx0 + 44.0f * s, cy + 28.0f * s,
                                   sel_car->make_name)) {
          mfx = cx0 + 50.0f * s;
        }
        list.text(mfx, cy + 8.0f * s,
                  std::to_string(sel_car->dealer.year) + "  " + sel_car->make_name + "  •  " +
                      drive_label(sel_car->drive_type_id),
                  ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite));
        ui4::class_badge(list, cx1 - 92.0f * s, cy + 4.0f * s, 24.0f * s, cls_name, shown_pi, cls);
        cy += 36.0f * s;

        // Compact Price & Swatch Preview PILL02 row
        const bool affordable = credits >= sel_car->dealer.base_cost;
        ui4::offwhite_strip(list, cx0, cy, cx1, cy + 28.0f * s, s, true);
        list.text(cx0 + 14.0f * s, cy + 7.0f * s, "AUTOSHOW PRICE", ui4::style(ui::Font::kB, 11.5f * s, ui4::kInk));
        list.text(cx1 - 14.0f * s, cy + 6.0f * s, ui4::format_credits(sel_car->dealer.base_cost),
                  ui4::style(ui::Font::kE, 15.5f * s, affordable ? ui4::kInk : ui4::kRed, ui::Align::kRight, true));
        cy += 36.0f * s;

        // 5 Floating PILL02 Stat Rows with DIRTTEXTURES/STATBARFILL & STATBAREMPTY (27px pitch, y=148..288)
        const std::pair<const char*, double> stats[5] = {
            {"SPEED", sel_car->speed_rating},
            {"HANDLING", sel_car->handling_rating},
            {"ACCELERATION", sel_car->accel_rating},
            {"LAUNCH", sel_car->launch_rating},
            {"BRAKING", sel_car->braking_rating},
        };
        for (int si = 0; si < 5; ++si) {
          ui4::pill(list, cx0, cy, cx1, cy + 25.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
          ui4::stat_bar(list, cx0 + 12.0f * s, cy + 2.0f * s, cw_card - 24.0f * s, 21.0f * s, stats[si].first,
                        stats[si].second, stats[si].second, false, s);
          cy += 28.0f * s;
        }
        cy += 4.0f * s;

        // Compact Power / Torque / Weight PILL26PX telemetry row
        char spec[128];
        const int hp = int(std::lround(sel_car->peak_power_kw * 1.34102));
        std::snprintf(spec, sizeof spec, "%d HP   •   %.0f NM   •   %.0f KG", hp, sel_car->peak_torque_nm,
                      sel_car->curb_weight_kg);
        ui4::pill(list, cx0, cy, cx1, cy + 26.0f * s, ui4::kBrushBlack, ui4::PillKind::kPill26);
        list.text(0.5f * (cx0 + cx1), cy + 6.0f * s, spec,
                  ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kCenter));
        cy += 30.0f * s;

        // Compact Factory Swatch Preview chips (pressing Y/A steps into 114_C_BUY_BUYCAR_COLOR)
        if (!sel_car->swatches.empty()) {
          const size_t nsw = std::min<size_t>(sel_car->swatches.size(), 10);
          ui4::pill(list, cx0, cy, cx1, cy + 32.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
          list.text(cx0 + 12.0f * s, cy + 9.0f * s, "COLOR", ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim));
          float sx = cx0 + 68.0f * s;
          std::array<float, 4> chip_uv{};
          const bool has_chip = ui4::texture("LIVERY/CAR_PAINT_GRID_CHIP", chip_uv);
          for (size_t i = 0; i < nsw; ++i) {
            const auto& sw = sel_car->swatches[i];
            const uint32_t col =
                ui::rgba(uint8_t((sw.rgb >> 16) & 0xFF), uint8_t((sw.rgb >> 8) & 0xFF), uint8_t(sw.rgb & 0xFF), 255);
            const bool act = (i == autoshow.selected_colour_index());
            list.rect(sx, cy + 6.0f * s, sx + 22.0f * s, cy + 26.0f * s, col);
            if (has_chip) list.image(sx, cy + 6.0f * s, sx + 22.0f * s, cy + 26.0f * s, chip_uv, ui::rgba(255, 255, 255, 64));
            if (act) list.frame(sx - 2.0f * s, cy + 4.0f * s, sx + 24.0f * s, cy + 28.0f * s, ui4::kPink, 2.0f * s);
            sx += 26.0f * s;
          }
        }
      }

      // Lower-left TitleTapeText @(91.2, 434.8) right above bottom car carousel!
      const std::string tape_lbl =
          autoshow.tutorial_mode() ? "CHOOSE YOUR FIRST CAR" : ("AUTOSHOW  //  " + active_make);
      ui4::title_tape(list, s, 88.0f * s, 424.0f * s, tape_lbl, ui4::kBrushBlack, 34.0f, 560.0f);

      // Bottom Tilted BIGPANE01 Car Carousel (y=490..620)
      const auto& vis = autoshow.visible_cars();
      if (!vis.empty()) {
        const float bot_y = 490.0f * s;
        const float card_ch = 114.0f * s;
        const int n = int(vis.size());
        const int sel = int(autoshow.selected_index());
        const int count = std::min(n, 5);
        const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
        const float gap = 12.0f * s;
        const float left_x = 68.0f * s, right_x = w - 68.0f * s;
        const float cw = (right_x - left_x - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
        float bx = left_x;
        for (int i = first; i < first + count; ++i) {
          const auto& c = vis[size_t(i)];
          const bool act = (i == sel);
          const float lift = 0.0f;
          list.add_hit(bx, bot_y - lift, bx + cw, bot_y + card_ch - lift, ui::kHitRow, i);
          draw_bigpane_card(list, bx, bot_y - lift, bx + cw, bot_y + card_ch - lift, s, act);

          const float th_h = 60.0f * s;
          const float th_w = th_h * 1.95f;
          const float th_x0 = bx + 0.5f * (cw - th_w);
          const float th_y0 = bot_y + 8.0f * s - lift;
          if (!ui4::car_thumbnail(list, th_x0, th_y0, th_x0 + th_w, th_y0 + th_h, c.dealer.id)) {
            ui4::manufacturer_logo(list, bx + cw * 0.5f - 24.0f * s, th_y0 + 10.0f * s, bx + cw * 0.5f + 24.0f * s,
                                   th_y0 + 48.0f * s, c.make_name, ui::rgba(255, 255, 255, 160));
          }

          const int cls =
              bands.empty() ? int(c.dealer.class_id) : garage::class_of(c.dealer.performance_index, bands);
          const std::string cname_cls = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
          const int cpi = bands.empty() ? int(std::ceil(c.dealer.performance_index * 999.0))
                                        : garage::shown_index(c.dealer.performance_index, bands);
          ui4::class_badge(list, bx + 8.0f * s, bot_y + 8.0f * s - lift, 17.0f * s, cname_cls, cpi, cls);

          std::string bname = car_name(c.dealer.media_name);
          if (bname.empty()) bname = c.display_name;
          ui::TextStyle cs = ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(bname, cs, cw - 16.0f * s, 8.5f * s);
          list.text(bx + 8.0f * s, bot_y + 72.0f * s - lift, bname, cs);

          ui::TextStyle ps = ui4::style(ui::Font::kB, 11.5f * s, act ? ui4::kYellow : ui4::kDim, ui::Align::kLeft, true);
          list.text(bx + 8.0f * s, bot_y + 92.0f * s - lift, ui4::format_credits(c.dealer.base_cost), ps);
          bx += cw + gap;
        }

        // TEXT_CURSOR @(652, 608.7)
        char cur[32];
        std::snprintf(cur, sizeof cur, "%d / %d", sel + 1, n);
        ui4::pill(list, 0.5f * w - 52.0f * s, bot_y + card_ch + 4.0f * s, 0.5f * w + 52.0f * s,
                  bot_y + card_ch + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
        list.text(0.5f * w, bot_y + card_ch + 7.5f * s, cur,
                  ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite, ui::Align::kCenter));
      }
    }
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  if (step == AutoshowStep::kMakeSelect) {
    ui4::button_bar(list, {{"A", "SELECT MAKE"}, {"DPAD_LR", "BROWSE"}, {"B", "EXIT"}}, w, h, 56.0f);
  } else if (step == AutoshowStep::kColorSelect) {
    ui4::button_bar(list, {{"A", "BUY CAR"}, {"DPAD_LR", "FACTORY COLOR"}, {"B", "BACK TO CARS"}}, w, h, 56.0f);
  } else {
    ui4::button_bar(list, {{"A", "CHOOSE COLOR / BUY"}, {"LB", "MAKE"}, {"Y", "FACTORY COLOR"}, {"B", "BACK"}},
                    w, h, 56.0f);
  }
}

void draw_upgrade_shop_hud(ui::List& list, float w, float h, const UpgradeShop& shop,
                           const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                           const std::string& car_title, const std::string& notice) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.20f);

  const float tilt_rad = scene_tilt_rad("527_C_UPG_UPGRADES", "Group_BLADE");

  // Top-left PlayerBar @(368, 41) + Discount tag — ZERO left vertical dark wall!
  draw_hub_player_bar(list, s, credits, car_title.empty() ? "DAK'S GARAGE" : car_title, discount_percent);

  // Slim top tilted category strip (LB / RB) tucked cleanly at y=62..90 so 3D car stays 100% open!
  static const std::vector<std::string> kCatTabs = {"ENGINE",     "PLATFORM",    "DRIVETRAIN", "TIRES/RIMS",
                                                    "AERO/BODY",  "CONVERSIONS", "AUTO UPG",   "TUNING"};
  {
    ui4::TiltScope tab_rot(list, 0.5f * w, 76.0f * s, tilt_rad * 0.6f);
    ui4::tab_strip(list, s, 56.0f * s, 62.0f * s, w - 56.0f * s, kCatTabs, int(shop.category()), true, ui4::kPink);
  }

  // ---- 527_C_UPG_UPGRADES: +3.0 deg Right Spec Card & Bottom Group_BLADE@(640,522) ----
  {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);

    // 1. Tilted Right Description & Live PI / 5-Stat Diff Card (x=848..1224, y=104..424)
    const float rx0 = w - 424.0f * s, rx1 = w - 56.0f * s;
    const float rw = rx1 - rx0;
    float ry = 104.0f * s;

    const auto& cur = shop.current_pi();
    const auto& prv = shop.preview_pi();
    const bool diff = shop.has_preview_pi();
    const std::string c_name =
        (bands.empty() || cur.class_id >= int(bands.size())) ? "?" : bands[size_t(cur.class_id)].name;
    const std::string p_name =
        (bands.empty() || prv.class_id >= int(bands.size())) ? "?" : bands[size_t(prv.class_id)].name;

    // Header PILL01 (#BFBFBF) + PILLCLIPON01
    ui4::pill(list, rx0, ry, rx1, ry + 28.0f * s, ui::rgba(0xBF, 0xBF, 0xBF, 240), ui4::PillKind::kMenu, false, true);
    list.text(rx0 + 14.0f * s, ry + 7.0f * s, "SPECIFICATIONS", ui4::style(ui::Font::kD, 14.0f * s, ui4::kInk));
    ry += 32.0f * s;

    // Live PI Comparison PILL02 row
    ui4::pill(list, rx0, ry, rx1, ry + 34.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    ui4::class_badge(list, rx0 + 10.0f * s, ry + 4.0f * s, 26.0f * s, c_name, cur.shown_pi, cur.class_id);
    if (diff && prv.shown_pi != cur.shown_pi) {
      const int dpi = prv.shown_pi - cur.shown_pi;
      const uint32_t dcol = dpi > 0 ? ui4::kGreen : ui4::kRed;
      ui4::opt_arrow(list, rx0 + 114.0f * s, ry + 17.0f * s, 14.0f * s, +1, dcol);
      ui4::class_badge(list, rx0 + 128.0f * s, ry + 4.0f * s, 26.0f * s, p_name, prv.shown_pi, prv.class_id);
      char db[24];
      std::snprintf(db, sizeof db, "%+d PI", dpi);
      ui4::brush(list, rx1 - 86.0f * s, ry + 4.0f * s, rx1 - 8.0f * s, ry + 30.0f * s, dcol);
      list.text(rx1 - 47.0f * s, ry + 10.0f * s, db,
                ui4::style(ui::Font::kE, 13.5f * s, ui4::kInk, ui::Align::kCenter));
    }
    ry += 38.0f * s;

    // 5 Floating PILL02 Stat Rows with STATBARFILL/STATBAREMPTY
    const struct {
      const char* name;
      double cur_v;
      double prv_v;
    } srows[5] = {
        {"SPEED", cur.stats.speed_rating, prv.stats.speed_rating},
        {"HANDLING", cur.stats.handling_rating, prv.stats.handling_rating},
        {"ACCEL", cur.stats.accel_rating, prv.stats.accel_rating},
        {"LAUNCH", cur.stats.launch_rating, prv.stats.launch_rating},
        {"BRAKING", cur.stats.braking_rating, prv.stats.braking_rating},
    };
    for (int i = 0; i < 5; ++i) {
      ui4::pill(list, rx0, ry, rx1, ry + 25.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
      ui4::stat_bar(list, rx0 + 12.0f * s, ry + 2.0f * s, rw - 24.0f * s, 21.0f * s, srows[i].name, srows[i].cur_v,
                    srows[i].prv_v, diff, s);
      ry += 28.0f * s;
    }
    ry += 4.0f * s;

    // Power / Weight / 0-60 floating PILL02 rows
    const int hp0 = int(std::lround(cur.stats.peak_power_kw * 1.34102));
    const int hp1 = int(std::lround(prv.stats.peak_power_kw * 1.34102));
    char line1[96], line2[96], line3[96];
    std::snprintf(line1, sizeof line1, "%d HP  ->  %d HP", hp0, hp1);
    std::snprintf(line2, sizeof line2, "%.0f KG  ->  %.0f KG", cur.stats.curb_weight_kg, prv.stats.curb_weight_kg);
    std::snprintf(line3, sizeof line3, "0-60: %.2fs   •   60-0: %.1fm   •   %.2fG", prv.stats.time_0_60_s,
                  prv.stats.brake_60_0_m, prv.stats.lat_g_60);

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    list.text(rx0 + 12.0f * s, ry + 6.0f * s, "POWER", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    list.text(rx1 - 12.0f * s, ry + 5.0f * s, line1,
              ui4::style(ui::Font::kE, 12.5f * s, hp1 > hp0 ? ui4::kGreen : ui4::kWhite, ui::Align::kRight));
    ry += 27.0f * s;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    list.text(rx0 + 12.0f * s, ry + 6.0f * s, "WEIGHT", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    list.text(rx1 - 12.0f * s, ry + 5.0f * s, line2,
              ui4::style(ui::Font::kE, 12.5f * s,
                         prv.stats.curb_weight_kg < cur.stats.curb_weight_kg ? ui4::kGreen : ui4::kWhite,
                         ui::Align::kRight));
    ry += 27.0f * s;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kPill26);
    list.text(0.5f * (rx0 + rx1), ry + 6.0f * s, line3,
              ui4::style(ui::Font::kB, 11.0f * s, ui4::kOffWhite, ui::Align::kCenter));

    // 2. Bottom +3.0 deg Group_BLADE@(640,522): TitleTapeText @(88.7, 435.7) + Horizontal Part/Tuning Blade!
    const std::string cat_title = std::string(shop_category_name(shop.category()));
    ui4::title_tape(list, s, 88.0f * s, 424.0f * s, cat_title, ui4::kBrushBlack, 34.0f, 540.0f);

    const float blade_y = 486.0f * s;
    const float blade_h = 132.0f * s;
    const float left_x = 68.0f * s, right_x = w - 68.0f * s;

    if (shop.category() == ShopCategory::kTuning && !shop.tuning_sliders().empty()) {
      // Authentic bottom +3.0 deg Tuning Blade with real Textures.zip/UI4/Tuning/*.xds icons + OPT_SLIDER_FRAME!
      const auto& sliders = shop.tuning_sliders();
      const int n = int(sliders.size());
      const int sel = int(std::min(shop.selected_slider(), sliders.size() - 1));
      const int count = std::min(n, 4);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float gap = 12.0f * s;
      const float cw = (right_x - left_x - gap * float(std::max(0, count - 1))) / float(std::max(1, count));

      float bx = left_x;
      for (int i = first; i < first + count; ++i) {
        const auto& sl = sliders[size_t(i)];
        const bool act = (i == sel);
        const float lift = 0.0f;
        list.add_hit(bx, blade_y - lift, bx + cw, blade_y + blade_h - lift, ui::kHitRow, i);
        draw_bigpane_card(list, bx, blade_y - lift, bx + cw, blade_y + blade_h - lift, s, act);

        // Real UI4/Tuning/I_TUN_* icon top-left of card
        const float ic = 44.0f * s;
        ui4::tuning_icon(list, bx + 10.0f * s, blade_y + 10.0f * s - lift, bx + 10.0f * s + ic,
                         blade_y + 10.0f * s + ic - lift, int(sl.category), sl.label,
                         sl.unlocked ? ui4::kWhite : ui::rgba(160, 165, 175, 160));

        const char* subcat = garage::tune_category_name(sl.category);
        list.text(bx + 62.0f * s, blade_y + 12.0f * s - lift, subcat,
                  ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kPinkHi : ui4::kDim));

        ui::TextStyle lt = ui4::style(ui::Font::kB, 13.5f * s,
                                      sl.unlocked ? ui4::kWhite : ui4::kDim);
        ui4::fit_text(sl.label, lt, cw - 72.0f * s, 9.0f * s);
        list.text(bx + 62.0f * s, blade_y + 28.0f * s - lift, sl.label, lt);

        char valbuf[48];
        if (!sl.unlocked) std::snprintf(valbuf, sizeof valbuf, "LOCKED");
        else if (sl.unit.empty()) std::snprintf(valbuf, sizeof valbuf, "%.2f", sl.value);
        else std::snprintf(valbuf, sizeof valbuf, "%.1f %s", sl.value, sl.unit.c_str());

        list.text(bx + cw - 14.0f * s, blade_y + 62.0f * s - lift, valbuf,
                  ui4::style(ui::Font::kE, 15.0f * s,
                             !sl.unlocked ? ui4::kDim : (sl.tuned ? ui4::kYellow : ui4::kWhite), ui::Align::kRight,
                             true));

        const double span = std::max(1e-6, sl.max_val - sl.min_val);
        const float frac = float(std::clamp((sl.value - sl.min_val) / span, 0.0, 1.0));
        const float def_f = float(std::clamp((sl.def_val - sl.min_val) / span, 0.0, 1.0));
        ui4::opt_slider(list, bx + 14.0f * s, blade_y + 92.0f * s - lift, bx + cw - 14.0f * s,
                        blade_y + 106.0f * s - lift, frac, s, !sl.unlocked ? ui4::kDim : (act ? ui4::kYellow : ui4::kPink),
                        act, def_f);
        bx += cw + gap;
      }

      char cur_s[32];
      std::snprintf(cur_s, sizeof cur_s, "%d / %d", sel + 1, n);
      ui4::pill(list, 0.5f * w - 52.0f * s, blade_y + blade_h + 4.0f * s, 0.5f * w + 52.0f * s,
                blade_y + blade_h + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
      list.text(0.5f * w, blade_y + blade_h + 7.5f * s, cur_s,
                ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite, ui::Align::kCenter));
    } else {
      // Authentic 527_C_UPG_UPGRADES Bottom BIGPANE01 Part Carousel with 479 real UI4/Upgrades/*.xds part icons!
      const auto rows = shop.visible_rows(discount_percent);
      const int n = int(rows.size());
      const int sel = n > 0 ? int(std::min(shop.selected_row(), rows.size() - 1)) : 0;
      const int count = std::min(n, 5);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float gap = 12.0f * s;
      const float cw = (right_x - left_x - gap * float(std::max(0, count - 1))) / float(std::max(1, count));

      std::array<float, 4> inst_uv{}, owned_uv{};
      const bool has_inst = ui4::texture("T:C_Map/C_UPG_Map/C_UPG_Icon_Installed", inst_uv) ||
                            ui4::texture("T:UI4/Upgrades/Installed_Icon", inst_uv) ||
                            ui4::texture("COMMON/TICK", inst_uv);
      const bool has_owned = ui4::texture("T:C_Map/C_UPG_Map/C_UPG_Icon_Owned", owned_uv) ||
                             ui4::texture("T:UI4/Upgrades/Owned_Icon", owned_uv);

      float bx = left_x;
      for (int i = first; i < first + count; ++i) {
        const auto& r = rows[size_t(i)];
        const bool act = (i == sel);
        const float lift = 0.0f;
        list.add_hit(bx, blade_y - lift, bx + cw, blade_y + blade_h - lift, ui::kHitRow, i);
        draw_bigpane_card(list, bx, blade_y - lift, bx + cw, blade_y + blade_h - lift, s, act);

        // Real UI4/Upgrades/* part icon centred in upper half of card!
        const float ic_sz = 64.0f * s;
        const float ic_x0 = bx + 0.5f * (cw - ic_sz);
        const float ic_y0 = blade_y + 10.0f * s - lift;
        if (!ui4::upgrade_part_icon(list, ic_x0, ic_y0, ic_x0 + ic_sz, ic_y0 + ic_sz, r.part_enum, r.level, r.is_stock,
                                    act ? ui4::kWhite : ui::rgba(225, 228, 236, 230))) {
          list.rect(ic_x0, ic_y0, ic_x0 + ic_sz, ic_y0 + ic_sz, ui::rgba(255, 255, 255, 16));
        }

        // Authentic C_UPG_Icon_Installed / C_UPG_Icon_Owned badge top-right of card
        if (r.fitted && has_inst) {
          list.image(bx + cw - 28.0f * s, blade_y + 8.0f * s - lift, bx + cw - 8.0f * s, blade_y + 28.0f * s - lift,
                     inst_uv, ui4::kGreen);
        } else if (r.owned && has_owned) {
          list.image(bx + cw - 28.0f * s, blade_y + 8.0f * s - lift, bx + cw - 8.0f * s, blade_y + 28.0f * s - lift,
                     owned_uv, ui4::kAmber);
        }

        // Tier arrows on focused card (real OPT_BAR_ARROW!)
        if (act) {
          ui4::opt_arrow(list, bx + 16.0f * s, ic_y0 + 0.5f * ic_sz, 14.0f * s, -1, ui4::kPinkHi);
          ui4::opt_arrow(list, bx + cw - 16.0f * s, ic_y0 + 0.5f * ic_sz, 14.0f * s, +1, ui4::kPinkHi);
        }

        // Slot name + tier label + price/state strip
        ui::TextStyle ls = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite);
        ui4::fit_text(r.label, ls, cw - 16.0f * s, 9.0f * s);
        list.text(bx + 8.0f * s, blade_y + 78.0f * s - lift, r.label, ls);

        ui::TextStyle ps = ui4::style(ui::Font::kB, 11.0f * s, act ? ui4::kOffWhite : ui4::kDim, ui::Align::kLeft, true);
        ui4::fit_text(r.part_label, ps, cw - 92.0f * s, 8.0f * s);
        list.text(bx + 8.0f * s, blade_y + 98.0f * s - lift, r.part_label, ps);

        const uint32_t scol = r.fitted ? ui4::kGreen : (r.owned ? ui4::kAmber : ui4::kYellow);
        ui::TextStyle ss = ui4::style(ui::Font::kE, 11.5f * s, act ? scol : scol, ui::Align::kRight, true);
        ui4::fit_text(r.state, ss, 86.0f * s, 8.0f * s);
        list.text(bx + cw - 8.0f * s, blade_y + 98.0f * s - lift, r.state, ss);
        bx += cw + gap;
      }

      if (n > 0) {
        char cur_s[32];
        std::snprintf(cur_s, sizeof cur_s, "%d / %d", sel + 1, n);
        ui4::pill(list, 0.5f * w - 52.0f * s, blade_y + blade_h + 4.0f * s, 0.5f * w + 52.0f * s,
                  blade_y + blade_h + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
        list.text(0.5f * w, blade_y + blade_h + 7.5f * s, cur_s,
                  ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite, ui::Align::kCenter));
      }
    }
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  ui4::button_bar(list,
                  {{"A", shop.category() == ShopCategory::kTuning ? "APPLY TUNE" : "FIT PART"},
                   {"LB", "CATEGORY"},
                   {"DPAD_LR", shop.category() == ShopCategory::kTuning ? "ADJUST" : "TIER"},
                   {"X", shop.category() == ShopCategory::kTuning ? "RESET" : "SELL PART"},
                   {"B", "EXIT"}},
                  w, h, 56.0f);
}

void draw_garage_hud(ui::List& list, float w, float h, const GarageBrowser& browser,
                     const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.20f);

  const float tilt_rad = scene_tilt_rad("106_C_CAR_LIST", "RotationHelper");

  // Top-left PlayerBar + Sort/Filter PILL01 chips (106_C_CAR_LIST)
  draw_hub_player_bar(list, s, credits, "MY GARAGE  //  " + std::to_string(browser.visible().size()) + " CARS");

  const std::string filt = browser.class_filter() < 0
                               ? "ALL CLASSES"
                               : (browser.class_filter() < int(bands.size())
                                      ? ("CLASS " + bands[size_t(browser.class_filter())].name)
                                      : "FILTERED");
  {
    ui4::TiltScope top_rot(list, 260.0f * s, 78.0f * s, tilt_rad);
    const float pill_y = 64.0f * s;
    ui4::pill(list, 56.0f * s, pill_y, 244.0f * s, pill_y + 26.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
    list.text(70.0f * s, pill_y + 6.0f * s, "SORT: " + std::string(garage_sort_name(browser.sort_mode())),
              ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite));
    ui4::pill(list, 252.0f * s, pill_y, 446.0f * s, pill_y + 26.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
    list.text(266.0f * s, pill_y + 6.0f * s, "FILTER: " + filt, ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite));
  }

  // Entire Garage details & car carousel tilted +3.0 deg (106_C_CAR_LIST + 104_C_CAR_DETAILS)!
  {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);

    // 1. Right-hand 104_C_CAR_DETAILS floating pill card (#BFBFBF PILL01 header + PILLCLIPON01 + #212121 PILL02 rows + SCROLLINDICATOR)
    if (const GarageEntry* sel_e = browser.selected_entry()) {
      const float rx1 = w - 58.0f * s;
      const float rw = 394.0f * s;
      const float rx0 = rx1 - rw;
      float cy = 44.0f * s;
      const std::string cname =
          (bands.empty() || sel_e->class_id >= int(bands.size())) ? "?" : bands[size_t(sel_e->class_id)].name;

      // #BFBFBF PILL01 Header + PILLCLIPON01
      ui4::pill(list, rx0, cy, rx1, cy + 30.0f * s, ui::rgba(0xBF, 0xBF, 0xBF, 242), ui4::PillKind::kMenu, false, true);
      list.text(rx0 + 14.0f * s, cy + 7.5f * s, "CAR DETAILS", ui4::style(ui::Font::kD, 14.5f * s, ui4::kInk));
      if (sel_e->is_current) {
        ui4::brush(list, rx1 - 122.0f * s, cy + 3.0f * s, rx1 - 6.0f * s, cy + 27.0f * s, ui4::kPink);
        list.text(rx1 - 64.0f * s, cy + 8.0f * s, "CURRENT CAR",
                  ui4::style(ui::Font::kE, 11.5f * s, ui4::kWhite, ui::Align::kCenter));
      }
      cy += 34.0f * s;

      // Car Name & Manufacturer badge row (104 Manufacturer badge + #F90064 PILL01 accent)
      ui4::pill(list, rx0, cy, rx1, cy + 56.0f * s, ui4::kCharcoalDark, ui4::PillKind::kPillBig);
      ui4::manufacturer_logo(list, rx1 - 76.0f * s, cy + 6.0f * s, rx1 - 12.0f * s, cy + 50.0f * s, sel_e->make_name);
      list.text(rx0 + 14.0f * s, cy + 8.0f * s, std::to_string(sel_e->year) + "  " + sel_e->make_name,
                ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
      std::string sel_label = car_name(sel_e->media_name);
      if (sel_label.empty() || sel_label.rfind("_&", 0) == 0) sel_label = sel_e->display_name;
      ui::TextStyle nm = ui4::style(ui::Font::kE, 19.5f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(sel_label, nm, rw - 96.0f * s, 11.5f * s);
      list.text(rx0 + 14.0f * s, cy + 26.0f * s, sel_label, nm);
      cy += 60.0f * s;

      // Floating #212121 PILL02 spec rows (28px pitch, matching 106_C_CAR_LIST)
      auto detail_row = [&](std::string_view k, std::string_view v, uint32_t vcol, bool pink_bg = false) {
        ui4::pill(list, rx0, cy, rx1, cy + 26.0f * s, pink_bg ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kPill02);
        list.text(rx0 + 14.0f * s, cy + 6.0f * s, k,
                  ui4::style(ui::Font::kD, 13.0f * s, pink_bg ? ui4::kWhite : ui4::kDim));
        list.text(rx1 - 14.0f * s, cy + 6.0f * s, v,
                  ui4::style(ui::Font::kB, 13.5f * s, vcol, ui::Align::kRight, true));
        cy += 29.0f * s;
      };

      // Class & PI row
      ui4::pill(list, rx0, cy, rx1, cy + 30.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
      list.text(rx0 + 14.0f * s, cy + 8.0f * s, "PERFORMANCE CLASS", ui4::style(ui::Font::kD, 13.0f * s, ui4::kDim));
      ui4::class_badge(list, rx1 - 92.0f * s, cy + 3.0f * s, 24.0f * s, cname, sel_e->shown_pi, sel_e->class_id);
      cy += 33.0f * s;

      detail_row("DRIVETRAIN", drive_label(sel_e->drive_type_id), ui4::kWhite);
      detail_row("GARAGE VALUE", ui4::format_credits(sel_e->value_cr), ui4::kWhite);
      detail_row("SELL VALUE (50%)", ui4::format_credits(sel_e->sell_cr), ui4::kYellow);

      // Authentic vertical SCROLLINDICATOR + SPEC_SHEEN2 beside details card
      ui4::scroll_indicator(list, rx1 + 6.0f * s, 46.0f * s, rx1 + 14.0f * s, cy - 4.0f * s,
                            int(browser.selected_index()), 1, std::max(2, int(browser.visible().size())), s);
    }

    // 2. Lower-left TitleTapeText @(91, 435) + Bottom Tilted BIGPANE01 Car Carousel (y=490..620)
    ui4::title_tape(list, s, 88.0f * s, 424.0f * s, "MY GARAGE", ui4::kBrushBlack, 34.0f, 440.0f);

    const auto& vis = browser.visible();
    if (!vis.empty()) {
      const float bot_y = 490.0f * s;
      const float card_h = 118.0f * s;
      const int n = int(vis.size());
      const int sel = int(browser.selected_index());
      const int count = std::min(n, 5);
      const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
      const float gap = 12.0f * s;
      const float left_x = 68.0f * s, right_x = w - 68.0f * s;
      const float cw = (right_x - left_x - gap * float(std::max(0, count - 1))) / float(std::max(1, count));
      float bx = left_x;
      for (int i = first; i < first + count; ++i) {
        const auto& e = vis[size_t(i)];
        const bool act = (i == sel);
        const float lift = 0.0f;
        list.add_hit(bx, bot_y - lift, bx + cw, bot_y + card_h - lift, ui::kHitRow, i);
        draw_bigpane_card(list, bx, bot_y - lift, bx + cw, bot_y + card_h - lift, s, act);

        const float th_h = 62.0f * s;
        const float th_w = th_h * 1.95f;
        const float th_x0 = bx + 0.5f * (cw - th_w);
        const float th_y0 = bot_y + 9.0f * s - lift;
        if (!ui4::car_thumbnail(list, th_x0, th_y0, th_x0 + th_w, th_y0 + th_h, e.car_id)) {
          ui4::manufacturer_logo(list, bx + cw * 0.5f - 24.0f * s, th_y0 + 12.0f * s, bx + cw * 0.5f + 24.0f * s,
                                 th_y0 + 50.0f * s, e.make_name, ui::rgba(255, 255, 255, 160));
        }

        const std::string cname =
            (bands.empty() || e.class_id >= int(bands.size())) ? "?" : bands[size_t(e.class_id)].name;
        ui4::class_badge(list, bx + 8.0f * s, bot_y + 8.0f * s - lift, 18.0f * s, cname, e.shown_pi, e.class_id);
        if (e.is_current) {
          ui4::brush(list, bx + cw - 74.0f * s, bot_y + 7.0f * s - lift, bx + cw - 6.0f * s, bot_y + 25.0f * s - lift,
                     ui4::kPink);
          list.text(bx + cw - 40.0f * s, bot_y + 11.0f * s - lift, "ACTIVE",
                    ui4::style(ui::Font::kE, 10.0f * s, ui4::kWhite, ui::Align::kCenter));
        }

        std::string card_label = car_name(e.media_name);
        if (card_label.empty() || card_label.rfind("_&", 0) == 0) card_label = e.display_name;
        ui::TextStyle ns = ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
        ui4::fit_text(card_label, ns, cw - 16.0f * s, 8.5f * s);
        list.text(bx + 8.0f * s, bot_y + 76.0f * s - lift, card_label, ns);

        ui::TextStyle sub = ui4::style(ui::Font::kB, 11.0f * s, act ? ui4::kYellow : ui4::kDim, ui::Align::kLeft, true);
        list.text(bx + 8.0f * s, bot_y + 96.0f * s - lift,
                  std::string(drive_label(e.drive_type_id)) + "  •  " + ui4::format_credits(e.value_cr), sub);
        bx += cw + gap;
      }

      char cur[32];
      std::snprintf(cur, sizeof cur, "%d / %d", sel + 1, n);
      ui4::pill(list, 0.5f * w - 52.0f * s, bot_y + card_h + 4.0f * s, 0.5f * w + 52.0f * s,
                bot_y + card_h + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kMenu);
      list.text(0.5f * w, bot_y + card_h + 7.5f * s, cur,
                ui4::style(ui::Font::kD, 13.0f * s, ui4::kWhite, ui::Align::kCenter));
    }
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  ui4::button_bar(list,
                  {{"A", "GET IN CAR"}, {"Y", "SORT"}, {"X", "FILTER"}, {"LB", "SELL CAR"}, {"B", "BACK"}},
                  w, h, 56.0f);
}

void draw_barn_find_hud(ui::List& list, float w, float h, const BarnFindCard& card) {
  if (!card.visible) return;
  const float s = h / 720.0f;
  const float cw = 540.0f * s, ch = 220.0f * s;
  const float cx = 0.5f * (w - cw), cy = h - ch - 84.0f * s;
  ui4::panel(list, cx, cy, cx + cw, cy + ch, s, ui4::kCharcoalDark, ui4::kAmber, 0, true);

  {
    ui4::TiltScope tilt(list, cx + 0.5f * cw, cy + 16.0f * s, ui4::kTiltRad * 0.6f);
    ui4::brush(list, cx - 12.0f * s, cy - 8.0f * s, cx + cw + 12.0f * s, cy + 36.0f * s, ui4::kAmber, true);
    list.text(cx + 0.5f * cw, cy + 4.0f * s, "BARN FIND RESTORED  //  " + card.barn_name,
              ui4::style(ui::Font::kE, 21.0f * s, ui4::kInk, ui::Align::kCenter));
  }

  const std::string title = (card.year > 0 ? (std::to_string(card.year) + " ") : "") + card.car_name;
  ui::TextStyle nm = ui4::style(ui::Font::kE, 26.0f * s, ui4::kWhite, ui::Align::kCenter, true);
  ui4::fit_text(title, nm, cw - 40.0f * s, 14.0f * s);
  list.text(cx + 0.5f * cw, cy + 56.0f * s, title, nm);

  ui4::class_badge(list, cx + 0.5f * cw - 46.0f * s, cy + 92.0f * s, 24.0f * s, card.class_name, card.shown_pi);

  const float bx = cx + 44.0f * s, bw = cw - 88.0f * s, by = cy + 132.0f * s;
  list.rect(bx, by, bx + bw, by + 10.0f * s, ui::rgba(255, 255, 255, 32));
  list.rect(bx, by, bx + bw * std::clamp(card.restore_progress, 0.0f, 1.0f), by + 10.0f * s, ui4::kGreen);

  list.text(cx + 0.5f * cw, cy + 160.0f * s, card.subtitle,
            ui4::style(ui::Font::kB, 13.5f * s, ui4::kDim, ui::Align::kCenter));

  ui4::button_bar(list, {{"A", "CONTINUE TO GARAGE"}}, w, h, 56.0f);
}

}  // namespace carview
