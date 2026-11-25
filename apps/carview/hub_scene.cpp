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
      user_orbited = false;
      manual_idle_s = 0.0f;
      break;
    case HubCameraFocus::kPaint:
      target_yaw = 0.80f;
      target_pitch = 0.21f;
      target_dist = 4.15f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.58f;
      target_pos[2] = -0.14f;
      auto_orbit = true;
      orbit_speed = 0.10f;
      user_orbited = false;
      manual_idle_s = 0.0f;
      orbit_blend = 1.0f;
      break;
    case HubCameraFocus::kRearThreeQuarter:
      target_yaw = 2.48f;
      target_pitch = 0.17f;
      target_dist = 5.05f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.55f;
      target_pos[2] = 0.0f;
      auto_orbit = false;
      user_orbited = false;
      manual_idle_s = 0.0f;
      break;
    case HubCameraFocus::kTurntable:
    default:
      target_yaw = 0.62f;
      target_pitch = 0.16f;
      target_dist = 5.15f;
      target_pos[0] = 0.0f;
      target_pos[1] = 0.54f;
      target_pos[2] = 0.0f;
      auto_orbit = true;
      orbit_speed = 0.16f;
      user_orbited = false;
      manual_idle_s = 0.0f;
      orbit_blend = 1.0f;
      break;
  }
}

void HubCamera::set_framing(float shift_x, float shift_y) {
  has_explicit_framing = true;
  target_lens_shift_x = std::clamp(shift_x, -0.35f, 0.35f);
  target_lens_shift_y = std::clamp(shift_y, -0.35f, 0.35f);
}

void HubCamera::nudge_orbit(float dyaw, float dpitch, float ddist) {
  auto_orbit = false;
  user_orbited = true;
  manual_idle_s = 0.0f;
  orbit_blend = 0.0f;
  target_yaw += dyaw;
  target_pitch = std::clamp(target_pitch + dpitch, 0.04f, 1.18f);
  target_dist = std::clamp(target_dist + ddist, 2.3f, 8.5f);
}

void HubCamera::step(float dt) {
  const float safe_dt = std::max(0.0f, dt);
  if (user_orbited && !auto_orbit && idle_orbit_delay > 0.0f) {
    manual_idle_s += safe_dt;
    if (manual_idle_s >= idle_orbit_delay) {
      auto_orbit = true;
      user_orbited = false;
      orbit_blend = 0.0f;
    }
  }
  if (auto_orbit) {
    orbit_blend = std::min(1.0f, orbit_blend + safe_dt * 0.85f);
    target_yaw += orbit_speed * orbit_blend * safe_dt;
  }
  const float alpha = 1.0f - std::exp(-7.0f * safe_dt);
  yaw += (target_yaw - yaw) * alpha;
  pitch += (target_pitch - pitch) * alpha;
  distance += (target_dist - distance) * alpha;
  for (int i = 0; i < 3; ++i) target[i] += (target_pos[i] - target[i]) * alpha;
  lens_shift_x += (target_lens_shift_x - lens_shift_x) * alpha;
  lens_shift_y += (target_lens_shift_y - lens_shift_y) * alpha;
}

std::array<float, 3> hub_background_color(HubRoom room) {
  switch (room) {
    case HubRoom::kAutoshow: return {0.74f, 0.78f, 0.84f};   // bright Horizon Autoshow pavilion cyclorama
    case HubRoom::kWorkshop: return {0.12f, 0.11f, 0.10f};   // Dak's warm industrial garage
    case HubRoom::kPaintShop: return {0.16f, 0.17f, 0.19f};  // neutral colour-true paint booth
    case HubRoom::kGarage: return {0.09f, 0.11f, 0.15f};     // Horizon festival bay
    case HubRoom::kBarnFind: return {0.13f, 0.10f, 0.07f};   // dusty golden barn light
    default: return {0.10f, 0.11f, 0.13f};
  }
}

WorldTriangles build_hub_stage(HubRoom room, float car_half_w, float car_half_l, float floor_y) {
  WorldTriangles wt;
  // Choose floor, plinth, accent ring, and cyclorama colours per Festival room.
  const bool is_autoshow = (room == HubRoom::kAutoshow);
  std::array<float, 3> floor_col = {0.18f, 0.19f, 0.22f};
  std::array<float, 3> plinth_col = {0.26f, 0.27f, 0.30f};
  std::array<float, 3> ring_col = {0.92f, 0.36f, 0.08f};  // Horizon signature coral-orange trim
  std::array<float, 3> wall_col = {0.11f, 0.12f, 0.15f};
  if (is_autoshow) {
    // Bright, crisp Horizon Autoshow architectural showroom floor & cool-white cyclorama.
    floor_col = {0.54f, 0.58f, 0.64f};
    plinth_col = {0.68f, 0.72f, 0.77f};
    ring_col = {0.96f, 0.12f, 0.44f};  // Horizon Autoshow magenta architectural accent
    wall_col = {0.72f, 0.76f, 0.82f};
  } else if (room == HubRoom::kWorkshop) {
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

  // Lean 48-segment circular stage cuts per-frame studio debris vertex uploads ~2x while
  // staying smoothly curved (~1,150 tris, well above the >= 200 stage quality threshold).
  constexpr int kSegs = 48;
  wt.positions.reserve(size_t(kSegs) * 28u * 3u);
  wt.normals.reserve(size_t(kSegs) * 28u * 3u);
  wt.group.reserve(size_t(kSegs) * 28u);

  auto clamp_col = [](std::array<float, 3> c) -> std::array<float, 3> {
    return {std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 0.0f, 1.0f), std::clamp(c[2], 0.0f, 1.0f)};
  };
  auto add_group = [&](std::array<float, 3> raw_col) -> uint8_t {
    const auto col = clamp_col(raw_col);
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

  // 1. Outer glossy showroom floor disc (radius 18 m with 3 softbox reflection sheen bands).
  const float y_outer = floor_y - 0.025f;
  const float floor_rings[4] = {3.25f, 7.2f, 12.6f, 18.0f};
  const float floor_shades[3] = {is_autoshow ? 1.10f : 1.00f, is_autoshow ? 0.96f : 0.78f,
                                 is_autoshow ? 0.82f : 0.56f};
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

  // 2. Polished turntable plinth disc (radius 3.12 m) + inner sheen halo + architectural accent rim.
  const float y_plinth = floor_y - 0.008f;
  const std::array<float, 3> plinth_inner =
      is_autoshow ? std::array<float, 3>{plinth_col[0] * 1.12f, plinth_col[1] * 1.12f, plinth_col[2] * 1.10f}
                  : plinth_col;
  for (int i = 0; i < kSegs; ++i) {
    const float a0 = 2.0f * kPi * float(i) / float(kSegs);
    const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
    const float r_sheen = 2.35f, r_mid = 3.12f, r_out = 3.24f;
    std::array<float, 3> c0{0.0f, y_plinth, 0.0f};
    std::array<float, 3> s0{r_sheen * std::cos(a0), y_plinth, r_sheen * std::sin(a0)};
    std::array<float, 3> s1{r_sheen * std::cos(a1), y_plinth, r_sheen * std::sin(a1)};
    std::array<float, 3> m0{r_mid * std::cos(a0), y_plinth, r_mid * std::sin(a0)};
    std::array<float, 3> m1{r_mid * std::cos(a1), y_plinth, r_mid * std::sin(a1)};
    std::array<float, 3> o0{r_out * std::cos(a0), y_plinth + 0.002f, r_out * std::sin(a0)};
    std::array<float, 3> o1{r_out * std::cos(a1), y_plinth + 0.002f, r_out * std::sin(a1)};
    push_tri(c0, s0, s1, plinth_inner);
    push_tri(s0, m0, m1, plinth_col);
    push_tri(s0, m1, s1, plinth_col);
    push_tri(m0, o0, o1, ring_col);
    push_tri(m0, o1, m1, ring_col);
  }

  // 3. Elliptical soft drop-shadow floor fan directly under the car's footprint
  //    (3 concentric rings fading from deep ambient occlusion under the chassis
  //    to the polished plinth tone outside the tyres).
  const float sx = std::max(0.75f, car_half_w * 1.06f);
  const float sz = std::max(1.65f, car_half_l * 1.04f);
  const float scales[4] = {0.0f, 0.72f, 1.00f, 1.24f};
  const float darks[3] = {is_autoshow ? 0.07f : 0.08f, is_autoshow ? 0.25f : 0.26f, is_autoshow ? 0.64f : 0.62f};
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

  // 4. Smooth studio cyclorama wall (radius 17.5 m, height 8.5 m) with inward smooth
  //    vertex normals and bright studio softbox gradient bands + horizon architectural trim.
  const float wall_y[5] = {y_outer, y_outer + 0.85f, y_outer + 3.2f, y_outer + 5.8f, y_outer + 8.5f};
  const float wall_shades[4] = {is_autoshow ? 0.86f : 0.72f, is_autoshow ? 1.06f : 1.08f,
                                is_autoshow ? 1.18f : 0.92f, is_autoshow ? 0.96f : 0.62f};
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
      const std::array<float, 3> n0{-c0, 0.14f, -s0};
      const std::array<float, 3> n1{-c1, 0.14f, -s1};
      std::array<float, 3> b0{r * c0, yb, r * s0};
      std::array<float, 3> b1{r * c1, yb, r * s1};
      std::array<float, 3> t0{r * c0, yt, r * s0};
      std::array<float, 3> t1{r * c1, yt, r * s1};
      push_tri_n(b0, t0, t1, n0, n0, n1, bcol);
      push_tri_n(b0, t1, b1, n0, n1, n1, bcol);
    }
  }
  // Luminous Horizon studio architectural light-strip band around upper pavilion cove.
  {
    const std::array<float, 3> strip_col =
        is_autoshow ? std::array<float, 3>{0.98f, 0.97f, 0.95f}
                    : std::array<float, 3>{wall_col[0] * 1.45f + ring_col[0] * 0.14f,
                                           wall_col[1] * 1.45f + ring_col[1] * 0.14f,
                                           wall_col[2] * 1.45f + ring_col[2] * 0.14f};
    const float r = 17.38f;
    const float yb = y_outer + 3.15f, yt = y_outer + 3.38f;
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

  // 5. Overhead warm-white architectural studio softbox ceiling strips in the Autoshow pavilion.
  if (is_autoshow) {
    const std::array<float, 3> softbox_col{0.98f, 0.97f, 0.95f};
    const std::array<float, 3> kDown{0.0f, -1.0f, 0.0f};
    const float y_ceil = floor_y + 5.8f;
    const float strip_xs[3] = {-3.2f, 0.0f, 3.2f};
    for (float sx_c : strip_xs) {
      const float hw_s = 0.55f, hl_s = 5.4f;
      std::array<float, 3> p0{sx_c - hw_s, y_ceil, -hl_s};
      std::array<float, 3> p1{sx_c + hw_s, y_ceil, -hl_s};
      std::array<float, 3> p2{sx_c + hw_s, y_ceil, hl_s};
      std::array<float, 3> p3{sx_c - hw_s, y_ceil, hl_s};
      push_tri_n(p0, p2, p1, kDown, kDown, kDown, softbox_col);
      push_tri_n(p0, p3, p2, kDown, kDown, kDown, softbox_col);
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
  const float fov_deg = (room == HubRoom::kAutoshow) ? 36.0f : 38.0f;
  build_perspective(fov_deg * (kPi / 180.0f), aspect, 0.1f, 120.0f, reverse_z, proj);

  // Subtle architectural shift-lens framing in the Autoshow pavilion keeps the hero car
  // cleanly elevated above the bottom carousel rail and balanced left of the right spec card
  // without tilting vertical lines.
  if (room == HubRoom::kAutoshow) {
    const float sx = cam.has_explicit_framing ? cam.lens_shift_x : 0.11f;
    const float sy = cam.has_explicit_framing ? cam.lens_shift_y : 0.16f;
    proj[8] += sx;
    proj[9] += sy;
  } else if (cam.has_explicit_framing) {
    proj[8] += cam.lens_shift_x;
    proj[9] += cam.lens_shift_y;
  }

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

  if (room == HubRoom::kAutoshow) {
    // Bright Horizon Autoshow studio lighting: overhead warm softbox key + crisp rim/fill
    // accenting body contours + glossy studio reflections without clipping white paint.
    out.key_light[0] = 0.38f;
    out.key_light[1] = 0.84f;
    out.key_light[2] = -0.39f;
    out.key_light[3] = 0.82f;
    out.fill_light[0] = -0.68f;
    out.fill_light[1] = 0.42f;
    out.fill_light[2] = 0.60f;
    out.fill_light[3] = 0.28f;
    out.options[0] = 0.56f;
    out.options[1] = 1.05f;
    out.tint[0] = 1.02f;
    out.tint[1] = 1.00f;
    out.tint[2] = 0.97f;
    out.reflect_tint[0] = 0.98f;
    out.reflect_tint[1] = 1.00f;
    out.reflect_tint[2] = 1.04f;
  } else if (room == HubRoom::kWorkshop) {
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
    case GarageSort::kYear: return "YEAR";
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
    double speed = 5.0, handling = 5.0, accel = 5.0, launch = 5.0, braking = 5.0;
    double power_kw = 0.0, torque_nm = 0.0, weight_kg = 0.0;
    double disp_cc = 0.0;
    std::string aspiration;
  };
  std::map<int64_t, Meta> metas;
  if (db && db->is_open()) {
    db->each("SELECT ID, COALESCE(DisplayName, ''), IconPathBase, ManufacturerCode FROM List_CarMake", {},
             [&](sqlite3_stmt* st) {
               const std::string dkey = col_text(st, 1);
               std::string n;
               if (!dkey.empty()) {
                 n = (dkey.rfind("_&", 0) == 0) ? game_text(dkey) : dkey;
                 if (n.rfind("_&", 0) == 0) n.clear();
               }
               if (n.empty()) n = col_text(st, 2);
               if (n.empty()) n = col_text(st, 3);
               make_names[sqlite3_column_int64(st, 0)] = n;
             },
             nullptr);
    db->each(
        "SELECT Id, MediaName, DisplayName, MakeID, Year, DriveTypeID, BaseCost, "
        "SpeedRating, HandlingRating, AccelerationRating, LaunchRating, BrakingRating, "
        "SimPeakPower, SimPeakTorque, CurbWeight FROM Data_Car",
        {},
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
          m.speed = sqlite3_column_double(st, 7);
          m.handling = sqlite3_column_double(st, 8);
          m.accel = sqlite3_column_double(st, 9);
          m.launch = sqlite3_column_double(st, 10);
          m.braking = sqlite3_column_double(st, 11);
          // FH1 stores SimPeakPower in 0.1 kW, SimPeakTorque in 100 Nm, CurbWeight in 100 kg.
          m.power_kw = sqlite3_column_double(st, 12) * 0.1;
          m.torque_nm = sqlite3_column_double(st, 13) * 100.0;
          m.weight_kg = sqlite3_column_double(st, 14) * 100.0;
          m.aspiration = "Naturally Aspirated";
          metas[id] = std::move(m);
        },
        nullptr);
    // Stock engine displacement & aspiration lookup.
    db->each(
        "SELECT ue.Ordinal, "
        "COALESCE((SELECT d.Disp FROM List_UpgradeEngineDisplacement d WHERE d.EngineID = ue.EngineID AND d.IsStock = 1 LIMIT 1), 0), "
        "EXISTS(SELECT 1 FROM List_UpgradeEngineTwinTurbo t WHERE t.EngineID = ue.EngineID AND t.IsStock = 1), "
        "EXISTS(SELECT 1 FROM List_UpgradeEngineSingleTurbo t WHERE t.EngineID = ue.EngineID AND t.IsStock = 1), "
        "EXISTS(SELECT 1 FROM List_UpgradeEngineCSC t WHERE t.EngineID = ue.EngineID AND t.IsStock = 1) OR "
        "EXISTS(SELECT 1 FROM List_UpgradeEngineDSC t WHERE t.EngineID = ue.EngineID AND t.IsStock = 1) "
        "FROM List_UpgradeEngine ue WHERE ue.IsStock = 1",
        {},
        [&](sqlite3_stmt* st) {
          const int64_t cid = sqlite3_column_int64(st, 0);
          auto it = metas.find(cid);
          if (it == metas.end()) return;
          it->second.disp_cc = sqlite3_column_double(st, 1);
          const bool has_tt = sqlite3_column_int(st, 2) != 0;
          const bool has_st = sqlite3_column_int(st, 3) != 0;
          const bool has_sc = sqlite3_column_int(st, 4) != 0;
          if (has_tt) it->second.aspiration = "Twin Turbo";
          else if (has_st) it->second.aspiration = "Turbo";
          else if (has_sc) it->second.aspiration = "Supercharged";
          else it->second.aspiration = "Naturally Aspirated";
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
    e.favorite = c.favorite;
    e.is_prize = c.is_prize;
    int64_t base_cost = 0;
    if (auto it = metas.find(c.car_id); it != metas.end()) {
      e.media_name = it->second.media;
      e.display_name = it->second.display.empty() ? car_name(it->second.media) : it->second.display;
      e.make_name = it->second.make.empty() ? "Other" : it->second.make;
      e.year = it->second.year;
      e.drive_type_id = it->second.drive;
      base_cost = it->second.base_cost;
      e.speed_rating = it->second.speed;
      e.handling_rating = it->second.handling;
      e.accel_rating = it->second.accel;
      e.launch_rating = it->second.launch;
      e.braking_rating = it->second.braking;
      e.peak_power_kw = it->second.power_kw;
      e.peak_torque_nm = it->second.torque_nm;
      e.curb_weight_kg = it->second.weight_kg;
      e.displacement_cc = it->second.disp_cc;
      e.aspiration = it->second.aspiration;
    } else {
      e.display_name = "Car #" + std::to_string(c.car_id);
      e.make_name = "Other";
      e.aspiration = "Naturally Aspirated";
    }
    // Use real upgraded cached stats when available (populated by refresh_car_pi).
    if (c.cached_stats.peak_power_kw > 0.0) {
      e.speed_rating = c.cached_stats.speed_rating;
      e.handling_rating = c.cached_stats.handling_rating;
      e.accel_rating = c.cached_stats.accel_rating;
      e.launch_rating = c.cached_stats.launch_rating;
      e.braking_rating = c.cached_stats.braking_rating;
      e.peak_power_kw = c.cached_stats.peak_power_kw;
      e.peak_torque_nm = c.cached_stats.peak_torque_nm;
      e.curb_weight_kg = c.cached_stats.curb_weight_kg;
      if (c.cached_stats.displacement_cc > 0.0) e.displacement_cc = c.cached_stats.displacement_cc;
      if (!c.cached_stats.aspiration.empty()) e.aspiration = c.cached_stats.aspiration;
    } else {
      // Reflect fitted aspiration conversion slots even before cached_stats is populated.
      auto slot_pos = [&](const char* col) {
        auto it = c.slots.find(col);
        return it != c.slots.end() && it->second > 0;
      };
      if (slot_pos("TwinTurbo")) e.aspiration = "Twin Turbo";
      else if (slot_pos("SingleTurbo")) e.aspiration = "Turbo";
      else if (slot_pos("SuperchargerCSC") || slot_pos("SuperchargerDSC")) e.aspiration = "Supercharged";
    }
    e.value_cr = base_cost + c.parts_value;
    e.sell_cr = garage::sell_car_value(base_cost, c.parts_value, c.is_prize);
    all_.push_back(std::move(e));
  }
  apply_filter_and_sort();
}

void GarageBrowser::apply_filter_and_sort() {
  visible_.clear();
  for (const auto& e : all_) {
    if (class_filter_ >= 0 && e.class_id != class_filter_) continue;
    if (favorites_only_ && !e.favorite) continue;
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
      case GarageSort::kYear:
        if (a.year != b.year) return a.year > b.year;
        return a.raw_pi > b.raw_pi;
      case GarageSort::kClass:
      default:
        if (a.class_id != b.class_id) return a.class_id < b.class_id;
        return a.raw_pi < b.raw_pi;
    }
  });
  selected_ = visible_.empty() ? 0 : std::min(selected_, visible_.size() - 1);
}

void GarageBrowser::move(int by) {
  remove_confirm_id_ = -1;
  if (tab_ == GarageTab::kRestorationBay) {
    if (restorations_.empty()) return;
    const int n = int(restorations_.size());
    selected_restoration_ = size_t(((int(selected_restoration_) + by) % n + n) % n);
    return;
  }
  if (visible_.empty()) return;
  const int n = int(visible_.size());
  selected_ = size_t(((int(selected_) + by) % n + n) % n);
}

void GarageBrowser::cycle_sort(int by) {
  remove_confirm_id_ = -1;
  const int n = int(GarageSort::kCount);
  sort_ = GarageSort(((int(sort_) + by) % n + n) % n);
  apply_filter_and_sort();
}

void GarageBrowser::cycle_class_filter(int by) {
  remove_confirm_id_ = -1;
  // Cycle: -1 (All), 0 (F), 1 (E), 2 (D), ..., 9 (R1), 10 (X).
  static constexpr int kCycle[] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  constexpr int n = int(sizeof(kCycle) / sizeof(kCycle[0]));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (kCycle[i] == class_filter_) idx = i;
  idx = ((idx + by) % n + n) % n;
  class_filter_ = kCycle[idx];
  selected_ = 0;
  apply_filter_and_sort();
}

void GarageBrowser::toggle_favorites_filter() {
  remove_confirm_id_ = -1;
  favorites_only_ = !favorites_only_;
  selected_ = 0;
  apply_filter_and_sort();
}

void GarageBrowser::set_favorites_filter(bool on) {
  remove_confirm_id_ = -1;
  favorites_only_ = on;
  selected_ = 0;
  apply_filter_and_sort();
}

bool GarageBrowser::toggle_favorite_selected(garage::Garage& g, const cars::PartsDb* db,
                                             const std::vector<garage::ClassBand>& bands) {
  const GarageEntry* e = selected_entry();
  if (!e) return false;
  const int64_t target_id = e->garage_id;
  const bool now_fav = g.toggle_favorite(target_id);
  refresh(g, db, bands);
  for (size_t i = 0; i < visible_.size(); ++i) {
    if (visible_[i].garage_id == target_id) {
      selected_ = i;
      break;
    }
  }
  return now_fav;
}

bool GarageBrowser::sell_selected(garage::Garage& g, const cars::PartsDb* db,
                                  const std::vector<garage::ClassBand>& bands, int64_t& credits,
                                  std::string& message) {
  remove_confirm_id_ = -1;
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

bool GarageBrowser::remove_selected(garage::Garage& g, const cars::PartsDb* db,
                                    const std::vector<garage::ClassBand>& bands, int64_t& credits,
                                    std::string& message, bool confirmed, bool allow_switch_active) {
  const GarageEntry* e = selected_entry();
  if (!e) {
    remove_confirm_id_ = -1;
    message = "no car selected";
    return false;
  }
  if (g.cars.size() <= 1) {
    remove_confirm_id_ = -1;
    message = "cannot remove your last car";
    return false;
  }
  if (e->is_current && !allow_switch_active) {
    remove_confirm_id_ = -1;
    message = "switch cars before removing your active car";
    return false;
  }
  if (!confirmed && remove_confirm_id_ != e->garage_id) {
    remove_confirm_id_ = e->garage_id;
    message = e->is_prize ? ("CONFIRM REMOVE PRIZE CAR " + e->display_name + " (0 CR)?")
                          : ("CONFIRM REMOVE " + e->display_name + " FROM GARAGE (0 CR)?");
    return false;
  }

  const int64_t target_id = e->garage_id;
  const std::string name = e->display_name;
  const bool prize = e->is_prize;
  auto it = std::find_if(g.cars.begin(), g.cars.end(), [target_id](const garage::Car& c) { return c.id == target_id; });
  if (it == g.cars.end()) {
    remove_confirm_id_ = -1;
    message = "car not found in garage";
    return false;
  }
  g.cars.erase(it);
  g.bought.erase(std::remove_if(g.bought.begin(), g.bought.end(),
                                [target_id](const garage::Bought& b) { return b.garage_id == target_id; }),
                 g.bought.end());
  g.club_shared_garage_ids.erase(
      std::remove(g.club_shared_garage_ids.begin(), g.club_shared_garage_ids.end(), target_id),
      g.club_shared_garage_ids.end());
  if (g.selected == target_id && !g.cars.empty()) {
    g.selected = g.cars.front().id;
  }
  // Removing a car yields 0 CR (prize/barn cars never refund Autoshow value).
  (void)credits;
  remove_confirm_id_ = -1;
  message = prize ? ("removed prize car " + name + " (0 Cr)") : ("removed " + name + " from garage (0 Cr)");
  refresh(g, db, bands);
  return true;
}

void GarageBrowser::set_tab(GarageTab t) {
  tab_ = t;
  remove_confirm_id_ = -1;
  if (tab_ == GarageTab::kRestorationBay && !restorations_.empty()) {
    selected_restoration_ = std::min(selected_restoration_, restorations_.size() - 1);
  }
}

void GarageBrowser::toggle_tab() {
  set_tab(tab_ == GarageTab::kMyCars ? GarageTab::kRestorationBay : GarageTab::kMyCars);
}

void GarageBrowser::set_restorations(std::vector<BarnRestorationEntry> items) {
  restorations_ = std::move(items);
  if (restorations_.empty()) {
    selected_restoration_ = 0;
  } else {
    selected_restoration_ = std::min(selected_restoration_, restorations_.size() - 1);
  }
}

int GarageBrowser::ready_restoration_count() const {
  int n = 0;
  for (const auto& r : restorations_) {
    if (r.ready_to_claim) ++n;
  }
  return n;
}

bool GarageBrowser::claim_selected_restoration(garage::Garage& g, const cars::PartsDb* db,
                                               const std::vector<garage::ClassBand>& bands,
                                               const AwardCarCallback& award_fn, std::string& claimed_barn_id_out,
                                               std::string& message) {
  claimed_barn_id_out.clear();
  if (restorations_.empty() || selected_restoration_ >= restorations_.size()) {
    message = "NO BARN FIND SELECTED";
    return false;
  }
  const BarnRestorationEntry entry = restorations_[selected_restoration_];
  if (!entry.ready_to_claim) {
    const int pct = int(std::lround(entry.progress() * 100.0f));
    message = "DAK IS STILL RESTORING " + entry.car_name + " (" + std::to_string(pct) + "%)";
    return false;
  }
  if (entry.car_id <= 0) {
    message = "INVALID BARN FIND CAR";
    return false;
  }
  std::string awarded_name;
  if (award_fn) {
    awarded_name = award_fn(entry.car_id, 0, false, /*is_prize=*/true);
  } else {
    std::map<std::string, int64_t> stock;
    double stock_pi = 0.5;
    int stock_cls = bands.empty() ? 2 : garage::class_of(stock_pi, bands);
    if (db) {
      std::string ignored;
      db->stock_slots(entry.car_id, stock, &ignored);
      std::vector<cars::DealerCar> dcars;
      if (db->dealer(dcars, nullptr, true)) {
        for (const auto& dc : dcars) {
          if (dc.id == entry.car_id) {
            stock_pi = dc.performance_index;
            stock_cls = bands.empty() ? int(dc.class_id) : garage::class_of(stock_pi, bands);
            break;
          }
        }
      }
    }
    garage::award_prize_car(g, entry.car_id, stock_pi, stock_cls, 0, stock, nullptr, false);
    awarded_name = entry.car_name;
  }
  if (awarded_name.empty()) awarded_name = entry.car_name;
  claimed_barn_id_out = entry.barn_id;
  restorations_.erase(restorations_.begin() + std::ptrdiff_t(selected_restoration_));
  if (!restorations_.empty()) {
    selected_restoration_ = std::min(selected_restoration_, restorations_.size() - 1);
  } else {
    selected_restoration_ = 0;
  }
  refresh(g, db, bands);
  message = "CLAIMED BARN FIND: " + awarded_name + " (PRIZE CAR)";
  return true;
}

bool GarageBrowser::claim_selected_restoration(garage::Garage& g, const cars::PartsDb* db,
                                               const std::vector<garage::ClassBand>& bands,
                                               std::string& claimed_barn_id_out, std::string& message) {
  return claim_selected_restoration(g, db, bands, AwardCarCallback{}, claimed_barn_id_out, message);
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
// Cached by scene pointer + node name to avoid per-frame scene-graph scans at 120 FPS.
float scene_tilt_rad(const char* scene_name, const char* root_node = "RotationHelper") {
  const fh1::UiScene* sc_ptr = ui4::scene(scene_name);
  if (!sc_ptr) return ui4::kTiltRad;
  struct CacheEntry {
    const fh1::UiScene* ptr = nullptr;
    const char* node = nullptr;
    float angle = ui4::kTiltRad;
  };
  static std::vector<CacheEntry> s_cache;
  for (const auto& e : s_cache) {
    if (e.ptr == sc_ptr && e.node == root_node) return e.angle;
  }
  float result = ui4::kTiltRad;
  SceneDraw sc(sc_ptr, true);
  if (const int n = sc.find(root_node); n >= 0) {
    const float a = -sc.angle(n);
    if (std::fabs(a) > 1e-3f && std::fabs(a) < 0.25f) result = a;
  }
  if (s_cache.size() < 32) s_cache.push_back({sc_ptr, root_node, result});
  return result;
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

UpgradeSpecStrings format_upgrade_spec_strings(const garage::CarStats& cur, const garage::CarStats& prv,
                                               bool metric_units) {
  UpgradeSpecStrings out;
  char b1[96], b2[96], b3[96], b4[128];
  if (metric_units) {
    const int kw0 = int(std::lround(cur.peak_power_kw));
    const int kw1 = int(std::lround(prv.peak_power_kw));
    const int nm0 = int(std::lround(cur.peak_torque_nm));
    const int nm1 = int(std::lround(prv.peak_torque_nm));
    const int kg0 = int(std::lround(cur.curb_weight_kg));
    const int kg1 = int(std::lround(prv.curb_weight_kg));
    // 100 km/h = 62.1371 mph -> 0-100 km/h scales linearly from 0-60 mph (x1.03562),
    // and 100-0 km/h braking distance scales quadratically (x1.0725).
    const double t_0_100_kmh = prv.time_0_60_s * 1.03562;
    const double brake_100_0_m = prv.brake_60_0_m * 1.0725;
    std::snprintf(b1, sizeof b1, "%d kW  ->  %d kW", kw0, kw1);
    std::snprintf(b2, sizeof b2, "%d NM  ->  %d NM", nm0, nm1);
    std::snprintf(b3, sizeof b3, "%d KG  ->  %d KG", kg0, kg1);
    std::snprintf(b4, sizeof b4, "0-100: %.2fs   •   100-0: %.1f M   •   %.2fG", t_0_100_kmh, brake_100_0_m,
                  prv.lat_g_60);
  } else {
    const int hp0 = int(std::lround(cur.peak_power_kw * 1.34102));
    const int hp1 = int(std::lround(prv.peak_power_kw * 1.34102));
    const int tq0 = int(std::lround(cur.peak_torque_nm * 0.737562));
    const int tq1 = int(std::lround(prv.peak_torque_nm * 0.737562));
    const int lb0 = int(std::lround(cur.curb_weight_kg * 2.20462));
    const int lb1 = int(std::lround(prv.curb_weight_kg * 2.20462));
    const double brake_60_0_ft = prv.brake_60_0_m * 3.28084;
    std::snprintf(b1, sizeof b1, "%d HP  ->  %d HP", hp0, hp1);
    std::snprintf(b2, sizeof b2, "%d LB-FT  ->  %d LB-FT", tq0, tq1);
    std::snprintf(b3, sizeof b3, "%d LBS  ->  %d LBS", lb0, lb1);
    std::snprintf(b4, sizeof b4, "0-60: %.2fs   •   60-0: %.0f FT   •   %.2fG", prv.time_0_60_s, brake_60_0_ft,
                  prv.lat_g_60);
  }
  out.power = b1;
  out.torque = b2;
  out.weight = b3;
  out.perf = b4;
  return out;
}

std::string format_autoshow_spec_line(double peak_power_kw, double peak_torque_nm, double curb_weight_kg,
                                      bool metric_units) {
  char spec[128];
  if (metric_units) {
    std::snprintf(spec, sizeof spec, "%d kW   •   %d NM   •   %d KG", int(std::lround(peak_power_kw)),
                  int(std::lround(peak_torque_nm)), int(std::lround(curb_weight_kg)));
  } else {
    std::snprintf(spec, sizeof spec, "%d HP   •   %d LB-FT   •   %d LBS",
                  int(std::lround(peak_power_kw * 1.34102)), int(std::lround(peak_torque_nm * 0.737562)),
                  int(std::lround(curb_weight_kg * 2.20462)));
  }
  return spec;
}

AutoshowLayout compute_autoshow_layout(float w, float h, AutoshowStep step, bool show_specs,
                                       bool show_filters) {
  AutoshowLayout lo{};
  const float s = std::max(0.25f, h / 720.0f);
  lo.scale = s;
  const float mx = 40.0f * s;

  lo.top_bar = {mx, 16.0f * s, std::max(mx + 200.0f * s, w - mx), 76.0f * s};

  if (show_filters && step == AutoshowStep::kCarSelect) {
    lo.filter_bar = {mx, 82.0f * s, std::min(w - mx, mx + 560.0f * s), 106.0f * s};
  }

  lo.button_bar = {mx, h - 68.0f * s, std::max(mx + 200.0f * s, w - mx), h - 34.0f * s};
  lo.carousel_rail = {mx, h - 182.0f * s, std::max(mx + 200.0f * s, w - mx), h - 74.0f * s};
  lo.rail_header = {mx, h - 208.0f * s, std::max(mx + 200.0f * s, w - mx), h - 185.0f * s};

  if (step == AutoshowStep::kColorSelect) {
    const float fin_w = std::min(308.0f * s, std::max(210.0f * s, lo.carousel_rail.width() * 0.27f));
    const float gap = 12.0f * s;
    lo.swatch_strip = {lo.carousel_rail.x0, lo.carousel_rail.y0,
                       std::max(lo.carousel_rail.x0 + 180.0f * s, lo.carousel_rail.x1 - fin_w - gap),
                       lo.carousel_rail.y1};
    lo.finish_strip = {lo.swatch_strip.x1 + gap, lo.carousel_rail.y0, lo.carousel_rail.x1, lo.carousel_rail.y1};
  }

  lo.spec_card_visible = show_specs;
  if (show_specs) {
    const float spec_w = std::min(344.0f * s, std::max(270.0f * s, w * 0.275f));
    const float sx1 = w - mx;
    const float sx0 = std::max(mx + 260.0f * s, sx1 - spec_w);
    const float sy0 = 84.0f * s;
    const float sy1 = std::min(lo.rail_header.y0 - 16.0f * s, 492.0f * s);
    lo.spec_card = {sx0, sy0, sx1, std::max(sy0 + 120.0f * s, sy1)};
  }

  const float hero_top = (lo.filter_bar.valid() ? lo.filter_bar.y1 : lo.top_bar.y1) + 10.0f * s;
  const float hero_bot = std::max(hero_top + 60.0f * s, lo.rail_header.y0 - 12.0f * s);
  const float hero_right = show_specs ? std::max(mx + 160.0f * s, lo.spec_card.x0 - 16.0f * s) : (w - mx);
  lo.hero_car_viewport = {mx, hero_top, hero_right, hero_bot};

  const float mw = std::min(510.0f * s, w - 2.0f * mx);
  const float mh = 268.0f * s;
  const float mx0 = 0.5f * (w - mw);
  const float my0 = std::clamp(0.5f * (hero_top + hero_bot - mh), lo.top_bar.y1 + 14.0f * s,
                               std::max(lo.top_bar.y1 + 14.0f * s, lo.rail_header.y0 - mh - 12.0f * s));
  lo.confirm_modal = {mx0, my0, mx0 + mw, my0 + mh};
  return lo;
}

namespace {

void draw_autoshow_rail_card(ui::List& list, float x0, float y0, float x1, float y1, float s, bool active) {
  // Sleek translucent Horizon Autoshow bottom-rail card with high contrast on the bright studio floor.
  list.rect(x0 + 2.0f * s, y0 + 3.0f * s, x1 + 2.0f * s, y1 + 3.0f * s, ui::rgba(0, 0, 0, 88));
  const uint32_t bg = active ? ui::rgba(28, 16, 28, 238) : ui::rgba(16, 18, 24, 214);
  list.rect(x0, y0, x1, y1, bg);
  std::array<float, 4> noise_uv{};
  if (ui4::texture("DIRTMASKS/BGNOISE01", noise_uv)) {
    list.image(x0, y0, x1, y1, noise_uv, ui::rgba(255, 255, 255, active ? 22 : 14));
  }
  if (active) {
    list.rect(x0, y0, x1, y0 + 3.5f * s, ui4::kPink);
    list.frame(x0, y0, x1, y1, ui4::kPink, 2.0f * s);
  } else {
    list.rect(x0, y0, x1, y0 + 1.5f * s, ui::rgba(255, 255, 255, 42));
    list.frame(x0, y0, x1, y1, ui::rgba(255, 255, 255, 24), 1.0f * s);
  }
}

void draw_autoshow_spec_card(ui::List& list, const AutoshowLayout& lo, const Autoshow& autoshow,
                             const std::vector<garage::ClassBand>& bands, int64_t credits, bool metric_units) {
  if (!lo.spec_card_visible || !lo.spec_card.valid()) return;
  const float s = lo.scale;
  const float rx0 = lo.spec_card.x0;
  const float rx1 = lo.spec_card.x1;
  const float rw = lo.spec_card.width();
  float cy = lo.spec_card.y0;

  const AutoshowCar* sel_car = autoshow.selected_car();
  if (!sel_car) {
    ui4::panel(list, rx0, cy, rx1, cy + 94.0f * s, s, ui::rgba(16, 18, 24, 224), ui4::kAmber, 0, true);
    list.text(0.5f * (rx0 + rx1), cy + 36.0f * s,
              autoshow.empty_reason().empty() ? "NO CARS AVAILABLE" : autoshow.empty_reason(),
              ui4::style(ui::Font::kE, 16.0f * s, ui4::kAmber, ui::Align::kCenter));
    return;
  }

  const bool affordable = credits >= sel_car->dealer.base_cost;
  const bool is_owned = autoshow.is_owned(sel_car->dealer.id);
  const float card_h = affordable ? 364.0f * s : 394.0f * s;
  const float card_y1 = std::min(lo.spec_card.y1, cy + card_h);
  ui4::panel(list, rx0, cy, rx1, card_y1, s, ui::rgba(16, 18, 24, 226), ui4::kPink, 0, true);

  const int cls = bands.empty() ? int(sel_car->dealer.class_id)
                                : garage::class_of(sel_car->dealer.performance_index, bands);
  const std::string cls_name = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
  const int shown_pi = bands.empty() ? int(std::ceil(sel_car->dealer.performance_index * 999.0))
                                     : garage::shown_index(sel_car->dealer.performance_index, bands);

  // 1. Header: Magenta accent strip + Year + Model Name + Make Logo
  std::string cname = car_name(sel_car->dealer.media_name);
  if (cname.empty()) cname = sel_car->display_name;
  list.rect(rx0, cy + 4.0f * s, rx1, cy + 36.0f * s, ui::rgba(249, 0, 100, 220));
  ui::TextStyle nm = ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite);
  ui4::fit_text(cname, nm, rw - 24.0f * s, 10.0f * s);
  list.text(rx0 + 12.0f * s, cy + 11.0f * s + 0.5f * (16.0f * s - nm.size), cname, nm);
  cy += 42.0f * s;

  // 2. Class/PI Badge + PI Delta vs Current Car + Drivetrain + Make Logo
  const AutoshowSpecDelta delta = autoshow.compare_car(*sel_car, bands);
  ui4::class_badge(list, rx0 + 12.0f * s, cy + 2.0f * s, 24.0f * s, cls_name, shown_pi, cls);

  if (delta.has_baseline && delta.pi_delta != 0) {
    char dpb[24];
    std::snprintf(dpb, sizeof dpb, "%+d PI", delta.pi_delta);
    const uint32_t pcol = delta.pi_delta > 0 ? ui4::kGreen : ui4::kRed;
    const float px0 = rx0 + 102.0f * s;
    list.rect(px0, cy + 4.0f * s, px0 + 62.0f * s, cy + 24.0f * s, ui::rgba(10, 12, 16, 210));
    list.frame(px0, cy + 4.0f * s, px0 + 62.0f * s, cy + 24.0f * s, pcol, 1.2f * s);
    list.text(px0 + 31.0f * s, cy + 8.0f * s, dpb,
              ui4::style(ui::Font::kE, 11.5f * s, pcol, ui::Align::kCenter));
  }

  // Drivetrain pill + Make badge right-aligned
  const char* dt = drive_label(sel_car->drive_type_id);
  const float dtx1 = rx1 - 12.0f * s;
  const float dtx0 = dtx1 - 46.0f * s;
  list.rect(dtx0, cy + 4.0f * s, dtx1, cy + 24.0f * s, ui::rgba(255, 255, 255, 24));
  list.text(0.5f * (dtx0 + dtx1), cy + 8.0f * s, dt,
            ui4::style(ui::Font::kE, 11.5f * s, ui4::kWhite, ui::Align::kCenter));

  if (is_owned) {
    const float ox1 = dtx0 - 6.0f * s;
    const float ox0 = ox1 - 58.0f * s;
    list.rect(ox0, cy + 4.0f * s, ox1, cy + 24.0f * s, ui4::kGreen);
    list.text(0.5f * (ox0 + ox1), cy + 8.0f * s, "OWNED",
              ui4::style(ui::Font::kE, 10.5f * s, ui4::kInk, ui::Align::kCenter));
  }
  cy += 32.0f * s;

  // 3. Subtitle / Comparison Baseline Header
  std::string sub_txt;
  if (delta.has_baseline) {
    sub_txt = "VS CURRENT: " + autoshow.comparison_car().display_name;
  } else {
    sub_txt = std::to_string(sel_car->dealer.year) + "  " + sel_car->make_name + "  •  FACTORY SPECS";
  }
  ui::TextStyle subs = ui4::style(ui::Font::kB, 10.5f * s, delta.has_baseline ? ui4::kYellow : ui4::kDim);
  ui4::fit_text(sub_txt, subs, rw - 24.0f * s, 8.5f * s);
  list.text(rx0 + 12.0f * s, cy + 2.0f * s, sub_txt, subs);
  cy += 18.0f * s;
  list.rect(rx0 + 12.0f * s, cy, rx1 - 12.0f * s, cy + 1.0f * s, ui::rgba(255, 255, 255, 32));
  cy += 6.0f * s;

  // 4. 5 Crisp FH1 Horizontal Stat Bars with Color-Coded Delta vs Player's Current Car
  const auto& cmp = autoshow.comparison_car();
  const struct {
    const char* name;
    double base_v;
    double cand_v;
  } stats[5] = {
      {"SPEED", delta.has_baseline ? cmp.speed_rating : sel_car->speed_rating, sel_car->speed_rating},
      {"HANDLING", delta.has_baseline ? cmp.handling_rating : sel_car->handling_rating, sel_car->handling_rating},
      {"ACCELERATION", delta.has_baseline ? cmp.accel_rating : sel_car->accel_rating, sel_car->accel_rating},
      {"LAUNCH", delta.has_baseline ? cmp.launch_rating : sel_car->launch_rating, sel_car->launch_rating},
      {"BRAKING", delta.has_baseline ? cmp.braking_rating : sel_car->braking_rating, sel_car->braking_rating},
  };
  for (int si = 0; si < 5; ++si) {
    list.rect(rx0 + 10.0f * s, cy, rx1 - 10.0f * s, cy + 22.0f * s,
              (si % 2 == 0) ? ui::rgba(255, 255, 255, 10) : ui::rgba(255, 255, 255, 4));
    ui4::stat_bar(list, rx0 + 14.0f * s, cy + 1.0f * s, rw - 28.0f * s, 20.0f * s, stats[si].name, stats[si].base_v,
                  stats[si].cand_v, delta.has_baseline, s);
    cy += 25.0f * s;
  }
  cy += 4.0f * s;

  // 5. Compact Technical Specs Grid: POWER, TORQUE, WEIGHT, DRIVETRAIN
  const float col_gap = 8.0f * s;
  const float half_w = 0.5f * (rw - 24.0f * s - col_gap);
  const float lx0 = rx0 + 12.0f * s;
  const float rcx0 = lx0 + half_w + col_gap;

  char pwr_buf[48], trq_buf[48], wt_buf[48];
  if (metric_units) {
    std::snprintf(pwr_buf, sizeof pwr_buf, "%d kW", int(std::lround(sel_car->peak_power_kw)));
    std::snprintf(trq_buf, sizeof trq_buf, "%d NM", int(std::lround(sel_car->peak_torque_nm)));
    std::snprintf(wt_buf, sizeof wt_buf, "%d KG", int(std::lround(sel_car->curb_weight_kg)));
  } else {
    std::snprintf(pwr_buf, sizeof pwr_buf, "%d HP", int(std::lround(sel_car->peak_power_kw * 1.34102)));
    std::snprintf(trq_buf, sizeof trq_buf, "%d LB-FT", int(std::lround(sel_car->peak_torque_nm * 0.737562)));
    std::snprintf(wt_buf, sizeof wt_buf, "%d LBS", int(std::lround(sel_car->curb_weight_kg * 2.20462)));
  }

  auto delta_col = [&](double d, bool lower_is_better = false) -> uint32_t {
    if (!delta.has_baseline || std::fabs(d) < 0.5) return ui4::kWhite;
    const bool better = lower_is_better ? (d < 0.0) : (d > 0.0);
    return better ? ui4::kGreen : ui4::kRed;
  };

  auto draw_kv_cell = [&](float cx, float y, float cw_c, const char* k, const char* v, uint32_t vcol) {
    list.rect(cx, y, cx + cw_c, y + 32.0f * s, ui::rgba(255, 255, 255, 14));
    list.text(cx + 8.0f * s, y + 4.0f * s, k, ui4::style(ui::Font::kB, 9.5f * s, ui4::kDim));
    ui::TextStyle vs = ui4::style(ui::Font::kE, 13.5f * s, vcol, ui::Align::kLeft, true);
    ui4::fit_text(v, vs, cw_c - 16.0f * s, 9.5f * s);
    list.text(cx + 8.0f * s, y + 15.5f * s, v, vs);
  };

  draw_kv_cell(lx0, cy, half_w, "POWER", pwr_buf, delta_col(delta.power_kw_delta));
  draw_kv_cell(rcx0, cy, half_w, "TORQUE", trq_buf, delta_col(delta.torque_nm_delta));
  cy += 36.0f * s;
  draw_kv_cell(lx0, cy, half_w, "WEIGHT", wt_buf, delta_col(delta.weight_kg_delta, true));
  draw_kv_cell(rcx0, cy, half_w, "DRIVETRAIN", dt, ui4::kWhite);
  cy += 40.0f * s;

  // 6. Price Bar + Affordability Guard
  ui4::offwhite_strip(list, lx0, cy, rx1 - 12.0f * s, cy + 30.0f * s, s, false);
  list.text(lx0 + 10.0f * s, cy + 8.0f * s, "PRICE", ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk));
  list.text(rx1 - 22.0f * s, cy + 6.5f * s, ui4::format_credits(sel_car->dealer.base_cost),
            ui4::style(ui::Font::kE, 16.0f * s, affordable ? ui4::kInk : ui4::kRed, ui::Align::kRight, true));
  cy += 34.0f * s;

  if (!affordable && cy + 24.0f * s <= card_y1) {
    const int64_t short_cr = autoshow.credits_shortfall(credits);
    list.rect(lx0, cy, rx1 - 12.0f * s, cy + 24.0f * s, ui::rgba(190, 32, 36, 230));
    const std::string warn = "INSUFFICIENT CREDITS (-" + ui4::format_credits(short_cr) + ")";
    ui::TextStyle ws = ui4::style(ui::Font::kE, 11.0f * s, ui4::kWhite, ui::Align::kCenter);
    ui4::fit_text(warn, ws, rw - 32.0f * s, 8.5f * s);
    list.text(0.5f * (lx0 + rx1 - 12.0f * s), cy + 6.5f * s, warn, ws);
  }
}

void draw_autoshow_confirm_modal(ui::List& list, float w, float h, const AutoshowLayout& lo, const Autoshow& autoshow,
                                 const std::vector<garage::ClassBand>& bands, int64_t credits) {
  const AutoshowCar* sel_car = autoshow.selected_car();
  if (!sel_car || !lo.confirm_modal.valid()) return;
  const float s = lo.scale;
  // Soft focus scrim strictly when confirmation modal is active.
  list.rect(0.0f, 0.0f, w, h, ui::rgba(8, 10, 14, 118));

  const float x0 = lo.confirm_modal.x0, y0 = lo.confirm_modal.y0;
  const float x1 = lo.confirm_modal.x1, y1 = lo.confirm_modal.y1;
  const float mw = lo.confirm_modal.width();
  ui4::panel(list, x0, y0, x1, y1, s, ui::rgba(18, 20, 26, 246), ui4::kPink, 0, true);

  // Magenta header
  list.rect(x0, y0 + 4.0f * s, x1, y0 + 40.0f * s, ui4::kPink);
  list.text(x0 + 18.0f * s, y0 + 12.0f * s, "CONFIRM PURCHASE", ui4::style(ui::Font::kE, 19.0f * s, ui4::kWhite));

  float cy = y0 + 52.0f * s;
  std::string cname = car_name(sel_car->dealer.media_name);
  if (cname.empty()) cname = sel_car->display_name;
  ui::TextStyle cn = ui4::style(ui::Font::kE, 20.0f * s, ui4::kWhite);
  ui4::fit_text(cname, cn, mw - 130.0f * s, 12.0f * s);
  list.text(x0 + 18.0f * s, cy, cname, cn);

  const int cls = bands.empty() ? int(sel_car->dealer.class_id)
                                : garage::class_of(sel_car->dealer.performance_index, bands);
  const std::string cls_name = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
  const int shown_pi = bands.empty() ? int(std::ceil(sel_car->dealer.performance_index * 999.0))
                                     : garage::shown_index(sel_car->dealer.performance_index, bands);
  ui4::class_badge(list, x1 - 104.0f * s, cy - 2.0f * s, 23.0f * s, cls_name, shown_pi, cls);
  cy += 28.0f * s;

  // Chosen Paint Swatch + Finish & Caliper summary
  if (const AutoshowSwatch* sw = autoshow.selected_swatch()) {
    const uint32_t sw_col =
        ui::rgba(uint8_t((sw->rgb >> 16) & 0xFF), uint8_t((sw->rgb >> 8) & 0xFF), uint8_t(sw->rgb & 0xFF), 255);
    list.rect(x0 + 18.0f * s, cy + 1.0f * s, x0 + 38.0f * s, cy + 19.0f * s, sw_col);
    list.frame(x0 + 18.0f * s, cy + 1.0f * s, x0 + 38.0f * s, cy + 19.0f * s, ui4::kWhite, 1.0f * s);
  }
  const std::string paint_desc = std::string("FINISH: ") + autoshow_paint_finish_name(autoshow.paint_finish()) +
                                 "   •   CALIPERS: " + autoshow.selected_caliper().name;
  list.text(x0 + 46.0f * s, cy + 3.0f * s, paint_desc, ui4::style(ui::Font::kB, 12.0f * s, ui4::kOffWhite));
  cy += 28.0f * s;

  // Cost breakdown rows
  const int64_t price = sel_car->dealer.base_cost;
  const bool affordable = credits >= price;
  const int64_t after_cr = credits - price;

  auto row = [&](const char* k, const std::string& v, uint32_t vc) {
    list.rect(x0 + 18.0f * s, cy, x1 - 18.0f * s, cy + 25.0f * s, ui::rgba(255, 255, 255, 12));
    list.text(x0 + 28.0f * s, cy + 5.5f * s, k, ui4::style(ui::Font::kB, 12.0f * s, ui4::kDim));
    list.text(x1 - 28.0f * s, cy + 5.0f * s, v,
              ui4::style(ui::Font::kE, 14.0f * s, vc, ui::Align::kRight, true));
    cy += 28.0f * s;
  };
  row("VEHICLE PRICE", ui4::format_credits(price), ui4::kWhite);
  row("CURRENT BALANCE", ui4::format_credits(credits), ui4::kOffWhite);
  row("BALANCE AFTER PURCHASE",
      affordable ? ui4::format_credits(after_cr) : ("INSUFFICIENT (-" + ui4::format_credits(price - credits) + ")"),
      affordable ? ui4::kGreen : ui4::kRed);

  // Confirm & Cancel action buttons
  const float btn_y0 = y1 - 52.0f * s;
  const float btn_y1 = y1 - 16.0f * s;
  const float bw = 0.5f * (mw - 48.0f * s);
  const float bx_ok = x0 + 18.0f * s;
  const float bx_no = bx_ok + bw + 12.0f * s;

  if (affordable) {
    list.add_hit(bx_ok, btn_y0, bx_ok + bw, btn_y1, ui::kHitConfirm, 0);
    list.rect(bx_ok, btn_y0, bx_ok + bw, btn_y1, ui4::kPink);
    list.text(bx_ok + 0.5f * bw, btn_y0 + 10.0f * s, "BUY & DRIVE",
              ui4::style(ui::Font::kE, 15.0f * s, ui4::kWhite, ui::Align::kCenter));
  } else {
    list.rect(bx_ok, btn_y0, bx_ok + bw, btn_y1, ui::rgba(70, 32, 36, 200));
    list.text(bx_ok + 0.5f * bw, btn_y0 + 10.0f * s, "INSUFFICIENT CR",
              ui4::style(ui::Font::kE, 14.0f * s, ui4::kDim, ui::Align::kCenter));
  }

  list.add_hit(bx_no, btn_y0, bx_no + bw, btn_y1, ui::kHitCancel, 0);
  list.rect(bx_no, btn_y0, bx_no + bw, btn_y1, ui::rgba(36, 40, 48, 230));
  list.frame(bx_no, btn_y0, bx_no + bw, btn_y1, ui::rgba(255, 255, 255, 65), 1.2f * s);
  list.text(bx_no + 0.5f * bw, btn_y0 + 10.0f * s, "CANCEL",
            ui4::style(ui::Font::kE, 15.0f * s, ui4::kWhite, ui::Align::kCenter));
}

}  // namespace

void draw_autoshow_hud(ui::List& list, float w, float h, const Autoshow& autoshow,
                       const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice,
                       bool metric_units) {
  const AutoshowStep step = autoshow.step();
  const bool show_filters = (step == AutoshowStep::kCarSelect && !autoshow.tutorial_mode());
  const AutoshowLayout lo = compute_autoshow_layout(w, h, step, autoshow.show_specs(), show_filters);
  const float s = lo.scale;

  const auto& makes = autoshow.makes();
  const std::string active_make =
      autoshow.all_cars_mode()
          ? std::string("ALL CARS")
          : (makes.empty() ? std::string("AUTOSHOW")
                           : makes[std::min(autoshow.selected_make_index(), makes.size() - 1)].name);

  // ---- 1. Top Bar: FH1 Magenta Header + Breadcrumb (Left) & Car Count + Credits (Top-Right) ----
  {
    const float tx0 = lo.top_bar.x0;
    const float ty0 = lo.top_bar.y0;
    const float tx1 = lo.top_bar.x1;

    // Magenta-accented HORIZON AUTOSHOW title header
    const std::string hdr_title = autoshow.tutorial_mode() ? "HORIZON AUTOSHOW  //  FIRST CAR" : "HORIZON AUTOSHOW";
    ui::TextStyle hts = ui4::style(ui::Font::kE, 20.0f * s, ui4::kWhite);
    const float hdr_w = std::min(380.0f * s, std::max(240.0f * s, ui::List::measure(hdr_title, hts) + 34.0f * s));
    ui4::fit_text(hdr_title, hts, hdr_w - 24.0f * s, 12.0f * s);
    ui4::brush(list, tx0 - 4.0f * s, ty0, tx0 + hdr_w, ty0 + 32.0f * s, ui4::kPink);
    list.text(tx0 + 12.0f * s, ty0 + 6.0f * s, hdr_title, hts);

    // Breadcrumb trail directly beneath header (MANUFACTURERS -> <MAKE> -> SELECT PAINT -> CONFIRM PURCHASE)
    const std::string crumb = autoshow.breadcrumb_text();
    const float by0 = ty0 + 36.0f * s;
    const float by1 = lo.top_bar.y1;
    ui::TextStyle bcs = ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite);
    const float max_crumb_w = std::max(220.0f * s, lo.top_bar.width() - 340.0f * s);
    ui4::fit_text(crumb, bcs, max_crumb_w - 24.0f * s, 9.0f * s);
    const float cw_b = std::min(max_crumb_w, ui::List::measure(crumb, bcs) + 26.0f * s);
    list.rect(tx0, by0, tx0 + cw_b, by1, ui::rgba(16, 18, 24, 214));
    list.rect(tx0, by0, tx0 + 3.5f * s, by1, ui4::kPink);
    list.text(tx0 + 12.0f * s, by0 + 5.5f * s, crumb, bcs);

    // Top-right Credits Balance + Car Count aligned cleanly with right margin (`tx1 = w - 40*s`)
    const float cr_w = 166.0f * s;
    const float cr_x0 = tx1 - cr_w;
    ui4::offwhite_strip(list, cr_x0, ty0 + 2.0f * s, tx1, ty0 + 32.0f * s, s, true);
    ui::TextStyle crs = ui4::style(ui::Font::kE, 16.0f * s, ui4::kInk, ui::Align::kRight, true);
    const std::string cr_str = ui4::format_credits(credits);
    ui4::fit_text(cr_str, crs, cr_w - 20.0f * s, 10.0f * s);
    list.text(tx1 - 12.0f * s, ty0 + 8.5f * s, cr_str, crs);

    // Car count pill immediately left of Credits strip
    char cnt_buf[48];
    if (step == AutoshowStep::kMakeSelect) {
      size_t total_cars = 0;
      for (const auto& mk : makes)
        if (mk.make_id == -4) total_cars = mk.car_count;
      if (total_cars == 0 && !makes.empty()) total_cars = makes[0].car_count;
      std::snprintf(cnt_buf, sizeof cnt_buf, "%zu MAKES  •  %zu CARS", makes.size(), total_cars);
    } else {
      std::snprintf(cnt_buf, sizeof cnt_buf, "%zu %s", autoshow.visible_cars().size(),
                    autoshow.visible_cars().size() == 1 ? "CAR" : "CARS");
    }
    ui::TextStyle cnts = ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite, ui::Align::kCenter);
    const float cnt_w = std::clamp(ui::List::measure(cnt_buf, cnts) + 24.0f * s, 84.0f * s, 210.0f * s);
    const float cnt_x1 = cr_x0 - 8.0f * s;
    const float cnt_x0 = cnt_x1 - cnt_w;
    list.rect(cnt_x0, ty0 + 2.0f * s, cnt_x1, ty0 + 32.0f * s, ui::rgba(16, 18, 24, 218));
    list.rect(cnt_x0, ty0 + 2.0f * s, cnt_x1, ty0 + 4.0f * s, ui4::kPink);
    list.text(0.5f * (cnt_x0 + cnt_x1), ty0 + 10.0f * s, cnt_buf, cnts);
  }

  // ---- 2. Compact Top-Left Sort / Class / Drivetrain Filter Bar (Car Select only) ----
  if (lo.filter_bar.valid()) {
    const std::string cfilt =
        autoshow.class_filter() < 0
            ? "ALL"
            : (autoshow.class_filter() < int(bands.size()) ? bands[size_t(autoshow.class_filter())].name : "?");
    const char* dfilt = autoshow.drivetrain_filter() == 1   ? "FWD"
                        : autoshow.drivetrain_filter() == 2 ? "RWD"
                        : autoshow.drivetrain_filter() == 3 ? "AWD"
                                                            : "ALL";
    const float fy0 = lo.filter_bar.y0;
    const float fy1 = lo.filter_bar.y1;
    float fx = lo.filter_bar.x0;
    auto chip = [&](int tab_idx, const std::string& label, float cw_chip, bool highlight) {
      list.add_hit(fx, fy0, fx + cw_chip, fy1, ui::kHitTab, tab_idx);
      list.rect(fx, fy0, fx + cw_chip, fy1, highlight ? ui4::kPink : ui::rgba(16, 18, 24, 212));
      list.frame(fx, fy0, fx + cw_chip, fy1, highlight ? ui4::kPinkHi : ui::rgba(255, 255, 255, 36), 1.0f * s);
      list.text(fx + 0.5f * cw_chip, fy0 + 6.0f * s, label,
                ui4::style(ui::Font::kB, 11.0f * s, ui4::kWhite, ui::Align::kCenter));
      fx += cw_chip + 6.0f * s;
    };
    chip(0, "SORT: " + std::string(autoshow_sort_name(autoshow.sort_mode())), 136.0f * s,
         autoshow.sort_mode() != AutoshowSort::kDefault);
    chip(1, "CLASS: " + cfilt, 112.0f * s, autoshow.class_filter() >= 0);
    chip(2, std::string("DRIVE: ") + dfilt, 114.0f * s, autoshow.drivetrain_filter() > 0);
    chip(3, autoshow.all_cars_mode() ? "ALL CARS: ON" : "ALL CARS", 104.0f * s, autoshow.all_cars_mode());
  }

  // ---- 3. Right-Docked Spec Comparison Card (Strictly outside Center Hero Car Viewport) ----
  draw_autoshow_spec_card(list, lo, autoshow, bands, credits, metric_units);

  // ---- 4. Bottom Horizontal FH1 Carousel Rail (Strictly Below Hero Car Viewport) ----
  {
    const float rx0 = lo.carousel_rail.x0;
    const float rx1 = lo.carousel_rail.x1;
    const float ry0 = lo.carousel_rail.y0;
    const float ry1 = lo.carousel_rail.y1;
    const float rail_h = lo.carousel_rail.height();

    // Sleek Rail Header strip (section label + page hint + sel/total counter)
    const float hy0 = lo.rail_header.y0;
    const float hy1 = lo.rail_header.y1;
    std::string rail_title =
        (step == AutoshowStep::kMakeSelect)  ? "MANUFACTURERS"
        : (step == AutoshowStep::kColorSelect) ? "FACTORY PAINT  &  FINISH"
                                               : (autoshow.tutorial_mode() ? "FIRST CAR MODELS" : (active_make + "  MODELS"));
    list.rect(rx0, hy0, rx0 + 230.0f * s, hy1, ui::rgba(16, 18, 24, 216));
    list.rect(rx0, hy0, rx0 + 4.0f * s, hy1, ui4::kPink);
    ui::TextStyle rts = ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite);
    ui4::fit_text(rail_title, rts, 212.0f * s, 9.0f * s);
    list.text(rx0 + 12.0f * s, hy0 + 5.0f * s, rail_title, rts);

    if (step == AutoshowStep::kMakeSelect) {
      if (!makes.empty()) {
        const int n = int(makes.size());
        const int sel = int(autoshow.selected_make_index());
        const int count = std::min(n, 6);
        const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));

        // Counter on right end of rail header
        char cur[40];
        std::snprintf(cur, sizeof cur, "%d / %d", sel + 1, n);
        list.rect(rx1 - 86.0f * s, hy0, rx1, hy1, ui::rgba(16, 18, 24, 216));
        list.text(rx1 - 43.0f * s, hy0 + 5.0f * s, cur,
                  ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite, ui::Align::kCenter));

        const float arrow_pad = 22.0f * s;
        const float strip_x0 = rx0 + arrow_pad;
        const float strip_x1 = rx1 - arrow_pad;
        const float gap = 10.0f * s;
        const float cw = (strip_x1 - strip_x0 - gap * float(std::max(0, count - 1))) / float(std::max(1, count));

        if (first > 0) ui4::opt_arrow(list, rx0 + 9.0f * s, ry0 + 0.5f * rail_h, 16.0f * s, -1, ui4::kPinkHi);
        if (first + count < n) ui4::opt_arrow(list, rx1 - 9.0f * s, ry0 + 0.5f * rail_h, 16.0f * s, +1, ui4::kPinkHi);

        float bx = strip_x0;
        for (int i = first; i < first + count; ++i) {
          const auto& mk = makes[size_t(i)];
          const bool act = (i == sel);
          list.add_hit(bx, ry0, bx + cw, ry1, ui::kHitRow, i);
          draw_autoshow_rail_card(list, bx, ry0, bx + cw, ry1, s, act);

          const float lh = 52.0f * s;
          const float lw = std::min(cw - 24.0f * s, lh * 1.55f);
          const float lx0 = bx + 0.5f * (cw - lw);
          const float ly0 = ry0 + 10.0f * s;
          if (!ui4::manufacturer_logo(list, lx0, ly0, lx0 + lw, ly0 + lh, mk.name,
                                      act ? ui4::kWhite : ui::rgba(222, 226, 234, 225))) {
            ui::TextStyle ms = ui4::style(ui::Font::kE, 16.0f * s, act ? ui4::kWhite : ui4::kOffWhite, ui::Align::kCenter);
            ui4::fit_text(mk.name, ms, cw - 16.0f * s, 9.5f * s);
            list.text(bx + 0.5f * cw, ry0 + 28.0f * s, mk.name, ms);
          }

          ui::TextStyle nm = ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kCenter);
          ui4::fit_text(mk.name, nm, cw - 14.0f * s, 8.5f * s);
          list.text(bx + 0.5f * cw, ry0 + 68.0f * s, mk.name, nm);

          list.text(bx + 0.5f * cw, ry0 + 86.0f * s,
                    std::to_string(mk.car_count) + (mk.car_count == 1 ? " MODEL" : " MODELS"),
                    ui4::style(ui::Font::kB, 10.5f * s, act ? ui4::kYellow : ui4::kDim, ui::Align::kCenter));
          bx += cw + gap;
        }
      }
    } else if (step == AutoshowStep::kColorSelect) {
      const AutoshowCar* sel_car = autoshow.selected_car();
      if (sel_car) {
        // Left part of bottom rail: Factory Color Swatch Strip
        const float sx0 = lo.swatch_strip.x0;
        const float sx1 = lo.swatch_strip.x1;
        list.rect(sx0, ry0, sx1, ry1, ui::rgba(16, 18, 24, 222));
        list.rect(sx0, ry0, sx1, ry0 + 2.5f * s, ui4::kPink);

        std::array<float, 4> chip_uv{}, met_uv{}, focus_uv{}, grad_uv{};
        const bool has_chip = ui4::texture("LIVERY/CAR_PAINT_GRID_CHIP", chip_uv);
        const bool has_met = ui4::texture("LIVERY/CAR_PAINT_GRID_METALLIC", met_uv);
        const bool has_focus = ui4::texture("LIVERY/CAR_PAINT_GRID_FOCUS", focus_uv);
        const bool has_grad = ui4::texture("LIVERY/CAR_PAINT_LIST_GRADIENT", grad_uv);
        if (has_grad) {
          list.image(sx0, ry0, sx1, ry1, grad_uv, ui::rgba(255, 255, 255, 34));
        }

        size_t first_sw = 0, nsw = 0;
        autoshow.swatch_window(14, first_sw, nsw);
        const size_t total_sw = sel_car->swatches.size();

        // Swatch status label in header
        if (total_sw > 0) {
          char swb[72];
          const auto* sel_sw = autoshow.selected_swatch();
          std::snprintf(swb, sizeof swb, "COLOR %zu / %zu%s%s", autoshow.selected_colour_index() + 1, total_sw,
                        (sel_sw && sel_sw->metallic) ? "  •  METALLIC" : "",
                        (sel_sw && sel_sw->has_stripe) ? "  •  STRIPE" : "");
          list.rect(rx1 - 246.0f * s, hy0, rx1, hy1, ui::rgba(16, 18, 24, 216));
          list.text(rx1 - 12.0f * s, hy0 + 5.0f * s, swb,
                    ui4::style(ui::Font::kB, 11.0f * s,
                               (sel_sw && sel_sw->has_stripe) ? ui4::kYellow : ui4::kOffWhite, ui::Align::kRight));
        }

        const float pad_lr = 24.0f * s;
        const float sw_gap = 6.0f * s;
        const float avail_w = std::max(80.0f * s, (sx1 - sx0) - 2.0f * pad_lr);
        const float sw_w = std::min(48.0f * s, (avail_w - sw_gap * float(std::max<size_t>(1, nsw) - 1)) /
                                                   float(std::max<size_t>(1, nsw)));
        const float sw_h = std::min(52.0f * s, rail_h - 32.0f * s);
        const float sw_y0 = ry0 + 0.5f * (rail_h - sw_h);

        if (first_sw > 0) ui4::opt_arrow(list, sx0 + 11.0f * s, ry0 + 0.5f * rail_h, 14.0f * s, -1, ui4::kPinkHi);
        if (first_sw + nsw < total_sw)
          ui4::opt_arrow(list, sx1 - 11.0f * s, ry0 + 0.5f * rail_h, 14.0f * s, +1, ui4::kPinkHi);

        float cx = sx0 + pad_lr;
        for (size_t k = 0; k < nsw; ++k) {
          const size_t sw_idx = first_sw + k;
          const auto& sw = sel_car->swatches[sw_idx];
          const uint32_t sw_col =
              ui::rgba(uint8_t((sw.rgb >> 16) & 0xFF), uint8_t((sw.rgb >> 8) & 0xFF), uint8_t(sw.rgb & 0xFF), 255);
          const bool active = (sw_idx == autoshow.selected_colour_index());
          const float y_c = sw_y0 - (active ? 3.0f * s : 0.0f);
          list.add_hit(cx, y_c, cx + sw_w, y_c + sw_h, ui4::kHitSwatch, int(sw_idx));
          list.rect(cx, y_c, cx + sw_w, y_c + sw_h, sw_col);
          if (sw.has_stripe) {
            const uint32_t st_col =
                sw.stripe_rgb != 0
                    ? ui::rgba(uint8_t((sw.stripe_rgb >> 16) & 0xFF), uint8_t((sw.stripe_rgb >> 8) & 0xFF),
                               uint8_t(sw.stripe_rgb & 0xFF), 255)
                    : ui4::kWhite;
            list.rect(cx + sw_w * 0.36f, y_c, cx + sw_w * 0.48f, y_c + sw_h, st_col);
            list.rect(cx + sw_w * 0.52f, y_c, cx + sw_w * 0.64f, y_c + sw_h, st_col);
          }
          if (sw.metallic && has_met) list.image(cx, y_c, cx + sw_w, y_c + sw_h, met_uv, ui::rgba(255, 255, 255, 92));
          else if (has_chip) list.image(cx, y_c, cx + sw_w, y_c + sw_h, chip_uv, ui::rgba(255, 255, 255, 64));
          if (active) {
            if (has_focus) {
              list.image(cx - 3.0f * s, y_c - 3.0f * s, cx + sw_w + 3.0f * s, y_c + sw_h + 3.0f * s, focus_uv,
                         ui4::kPink);
            }
            list.frame(cx - 2.5f * s, y_c - 2.5f * s, cx + sw_w + 2.5f * s, y_c + sw_h + 2.5f * s, ui4::kPink, 2.5f * s);
            list.frame(cx - 1.0f * s, y_c - 1.0f * s, cx + sw_w + 1.0f * s, y_c + sw_h + 1.0f * s, ui4::kWhite, 1.0f * s);
          } else {
            list.frame(cx, y_c, cx + sw_w, y_c + sw_h, ui::rgba(0, 0, 0, 140), 1.0f * s);
          }
          cx += sw_w + sw_gap;
        }

        // Right part of bottom rail: Open-Engine Finish & Caliper Options!
        if (lo.finish_strip.valid()) {
          const float fx0 = lo.finish_strip.x0;
          const float fx1 = lo.finish_strip.x1;
          list.rect(fx0, ry0, fx1, ry1, ui::rgba(16, 18, 24, 224));
          list.rect(fx0, ry0, fx1, ry0 + 2.5f * s, ui4::kPink);

          const float row_h = 40.0f * s;
          const float r1_y0 = ry0 + 10.0f * s;
          const float r2_y0 = r1_y0 + row_h + 8.0f * s;

          // Finish selector (kHitRegion index 0)
          list.add_hit(fx0 + 8.0f * s, r1_y0, fx1 - 8.0f * s, r1_y0 + row_h, ui4::kHitRegion, 0);
          list.rect(fx0 + 8.0f * s, r1_y0, fx1 - 8.0f * s, r1_y0 + row_h, ui::rgba(255, 255, 255, 16));
          list.text(fx0 + 16.0f * s, r1_y0 + 5.0f * s, "FINISH  [LB/RB]", ui4::style(ui::Font::kB, 9.5f * s, ui4::kDim));
          list.text(fx0 + 16.0f * s, r1_y0 + 19.0f * s, autoshow_paint_finish_name(autoshow.paint_finish()),
                    ui4::style(ui::Font::kE, 14.0f * s,
                               autoshow.paint_finish() == AutoshowPaintFinish::kFactory ? ui4::kWhite : ui4::kYellow));
          ui4::opt_arrow(list, fx1 - 22.0f * s, r1_y0 + 0.5f * row_h, 12.0f * s, +1, ui4::kPinkHi);

          // Caliper selector (kHitRegion index 1)
          const auto& cal = autoshow.selected_caliper();
          list.add_hit(fx0 + 8.0f * s, r2_y0, fx1 - 8.0f * s, r2_y0 + row_h, ui4::kHitRegion, 1);
          list.rect(fx0 + 8.0f * s, r2_y0, fx1 - 8.0f * s, r2_y0 + row_h, ui::rgba(255, 255, 255, 16));
          list.text(fx0 + 16.0f * s, r2_y0 + 5.0f * s, "CALIPERS  [UP/DN]", ui4::style(ui::Font::kB, 9.5f * s, ui4::kDim));
          if (cal.active) {
            const uint32_t ccol = ui::rgba(uint8_t((cal.rgb >> 16) & 0xFF), uint8_t((cal.rgb >> 8) & 0xFF),
                                           uint8_t(cal.rgb & 0xFF), 255);
            list.rect(fx0 + 16.0f * s, r2_y0 + 20.0f * s, fx0 + 30.0f * s, r2_y0 + 33.0f * s, ccol);
            list.frame(fx0 + 16.0f * s, r2_y0 + 20.0f * s, fx0 + 30.0f * s, r2_y0 + 33.0f * s, ui4::kWhite, 1.0f * s);
          }
          list.text(fx0 + (cal.active ? 36.0f : 16.0f) * s, r2_y0 + 19.0f * s, cal.name,
                    ui4::style(ui::Font::kE, 14.0f * s, cal.active ? ui4::kYellow : ui4::kWhite));
          ui4::opt_arrow(list, fx1 - 22.0f * s, r2_y0 + 0.5f * row_h, 12.0f * s, +1, ui4::kPinkHi);
        }
      }
    } else {
      // ---- kCarSelect: Bottom Horizontal FH1 Model Carousel Rail ----
      const auto& vis = autoshow.visible_cars();
      if (!vis.empty()) {
        const int n = int(vis.size());
        const int sel = int(autoshow.selected_index());
        const int count = std::min(n, 5);
        const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));

        char cur[40];
        std::snprintf(cur, sizeof cur, "%d / %d", sel + 1, n);
        list.rect(rx1 - 86.0f * s, hy0, rx1, hy1, ui::rgba(16, 18, 24, 216));
        list.text(rx1 - 43.0f * s, hy0 + 5.0f * s, cur,
                  ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite, ui::Align::kCenter));

        const float arrow_pad = 22.0f * s;
        const float strip_x0 = rx0 + arrow_pad;
        const float strip_x1 = rx1 - arrow_pad;
        const float gap = 10.0f * s;
        const float cw = (strip_x1 - strip_x0 - gap * float(std::max(0, count - 1))) / float(std::max(1, count));

        if (first > 0) ui4::opt_arrow(list, rx0 + 9.0f * s, ry0 + 0.5f * rail_h, 16.0f * s, -1, ui4::kPinkHi);
        if (first + count < n) ui4::opt_arrow(list, rx1 - 9.0f * s, ry0 + 0.5f * rail_h, 16.0f * s, +1, ui4::kPinkHi);

        float bx = strip_x0;
        for (int i = first; i < first + count; ++i) {
          const auto& c = vis[size_t(i)];
          const bool act = (i == sel);
          const bool affordable = credits >= c.dealer.base_cost;
          list.add_hit(bx, ry0, bx + cw, ry1, ui::kHitRow, i);
          draw_autoshow_rail_card(list, bx, ry0, bx + cw, ry1, s, act);

          const float th_h = 54.0f * s;
          const float th_w = std::min(cw - 16.0f * s, th_h * 1.95f);
          const float th_x0 = bx + 0.5f * (cw - th_w);
          const float th_y0 = ry0 + 8.0f * s;
          if (!ui4::car_thumbnail(list, th_x0, th_y0, th_x0 + th_w, th_y0 + th_h, c.dealer.id)) {
            ui4::manufacturer_logo(list, bx + 0.5f * cw - 22.0f * s, th_y0 + 8.0f * s, bx + 0.5f * cw + 22.0f * s,
                                   th_y0 + 44.0f * s, c.make_name, ui::rgba(255, 255, 255, 175));
          }

          const int cls =
              bands.empty() ? int(c.dealer.class_id) : garage::class_of(c.dealer.performance_index, bands);
          const std::string cname_cls = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
          const int cpi = bands.empty() ? int(std::ceil(c.dealer.performance_index * 999.0))
                                        : garage::shown_index(c.dealer.performance_index, bands);
          ui4::class_badge(list, bx + 8.0f * s, ry0 + 7.0f * s, 16.0f * s, cname_cls, cpi, cls);

          if (autoshow.is_owned(c.dealer.id)) {
            list.rect(bx + cw - 56.0f * s, ry0 + 7.0f * s, bx + cw - 6.0f * s, ry0 + 22.0f * s, ui4::kGreen);
            list.text(bx + cw - 31.0f * s, ry0 + 9.5f * s, "OWNED",
                      ui4::style(ui::Font::kE, 9.0f * s, ui4::kInk, ui::Align::kCenter));
          }

          std::string bname = car_name(c.dealer.media_name);
          if (bname.empty()) bname = c.display_name;
          const std::string yr_name =
              (c.dealer.year > 0 && bname.find(std::to_string(c.dealer.year)) == std::string::npos)
                  ? (std::to_string(c.dealer.year) + " " + bname)
                  : bname;
          ui::TextStyle cs = ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(yr_name, cs, cw - 16.0f * s, 8.5f * s);
          list.text(bx + 8.0f * s, ry0 + 67.0f * s, yr_name, cs);

          const uint32_t pcol = !affordable ? ui4::kRed : (act ? ui4::kYellow : ui4::kOffWhite);
          ui::TextStyle ps = ui4::style(ui::Font::kE, 11.5f * s, pcol, ui::Align::kLeft, true);
          list.text(bx + 8.0f * s, ry0 + 86.0f * s, ui4::format_credits(c.dealer.base_cost), ps);
          bx += cw + gap;
        }
      } else {
        list.rect(rx0, ry0, rx1, ry1, ui::rgba(16, 18, 24, 212));
        list.text(0.5f * (rx0 + rx1), ry0 + 44.0f * s,
                  autoshow.empty_reason().empty() ? "NO MATCHING VEHICLES" : autoshow.empty_reason(),
                  ui4::style(ui::Font::kE, 16.0f * s, ui4::kAmber, ui::Align::kCenter));
      }
    }
  }

  // ---- 5. Purchase Confirmation Modal (when active) ----
  if (autoshow.buy_confirm_open()) {
    draw_autoshow_confirm_modal(list, w, h, lo, autoshow, bands, credits);
  }

  // ---- 6. Optional Toast Notice (docked bottom-right cleanly above bottom margin) ----
  if (!notice.empty()) {
    const float nw = 380.0f * s;
    ui4::brush(list, w - nw - 40.0f * s, lo.button_bar.y0 - 2.0f * s, w - 40.0f * s, lo.button_bar.y1 - 2.0f * s,
               ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, nw - 24.0f * s, 9.0f * s);
    list.text(w - 52.0f * s, lo.button_bar.y0 + 7.0f * s, notice, ns);
  }

  // ---- 7. Bottom Controller / Keyboard Button Bar ----
  if (autoshow.buy_confirm_open()) {
    ui4::button_bar(list, {{"A", "CONFIRM BUY"}, {"B", "CANCEL"}}, w, h, 40.0f);
  } else if (step == AutoshowStep::kMakeSelect) {
    ui4::button_bar(list,
                    {{"A", "SELECT MAKE"}, {"LB", "PAGE"}, {"Y", "SPECS / CAM"}, {"RS", "ORBIT"}, {"B", "EXIT"}},
                    w, h, 40.0f);
  } else if (step == AutoshowStep::kColorSelect) {
    ui4::button_bar(list,
                    {{"A", "BUY CAR"},
                     {"X", "TEST DRIVE"},
                     {"LB", "FINISH"},
                     {"Y", "SPECS / CAM"},
                     {"RS", "ORBIT"},
                     {"B", "BACK"}},
                    w, h, 40.0f);
  } else {
    ui4::button_bar(list,
                    {{"A", "SELECT PAINT"},
                     {"X", "SORT / FILTER"},
                     {"Y", "SPECS / CAM"},
                     {"LB", "PAGE"},
                     {"RS", "ORBIT"},
                     {"B", "BACK"}},
                    w, h, 40.0f);
  }
}

void draw_upgrade_shop_hud(ui::List& list, float w, float h, const UpgradeShop& shop,
                           const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                           const std::string& car_title, const std::string& notice, bool metric_units) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.20f);

  const float tilt_rad = scene_tilt_rad("527_C_UPG_UPGRADES", "Group_BLADE");

  const int eff_disc = shop.effective_discount_percent(discount_percent);

  // Top-left PlayerBar @(368, 41) + Discount tag — ZERO left vertical dark wall!
  draw_hub_player_bar(list, s, credits, car_title.empty() ? "DAK'S GARAGE" : car_title, eff_disc);

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

    // Power / Torque / Weight / Perf floating PILL02 rows respecting global Metric/Imperial Units
    const UpgradeSpecStrings specs = format_upgrade_spec_strings(cur.stats, prv.stats, metric_units);
    const bool power_up = prv.stats.peak_power_kw > cur.stats.peak_power_kw + 0.05;
    const bool torque_up = prv.stats.peak_torque_nm > cur.stats.peak_torque_nm + 0.05;
    const bool weight_down = prv.stats.curb_weight_kg < cur.stats.curb_weight_kg - 0.5;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    list.text(rx0 + 12.0f * s, ry + 6.0f * s, "POWER", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    list.text(rx1 - 12.0f * s, ry + 5.0f * s, specs.power,
              ui4::style(ui::Font::kE, 12.5f * s, power_up ? ui4::kGreen : ui4::kWhite, ui::Align::kRight));
    ry += 27.0f * s;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    list.text(rx0 + 12.0f * s, ry + 6.0f * s, "TORQUE", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    list.text(rx1 - 12.0f * s, ry + 5.0f * s, specs.torque,
              ui4::style(ui::Font::kE, 12.5f * s, torque_up ? ui4::kGreen : ui4::kWhite, ui::Align::kRight));
    ry += 27.0f * s;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
    list.text(rx0 + 12.0f * s, ry + 6.0f * s, "WEIGHT", ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    list.text(rx1 - 12.0f * s, ry + 5.0f * s, specs.weight,
              ui4::style(ui::Font::kE, 12.5f * s, weight_down ? ui4::kGreen : ui4::kWhite, ui::Align::kRight));
    ry += 27.0f * s;

    ui4::pill(list, rx0, ry, rx1, ry + 24.0f * s, ui4::kBrushBlack, ui4::PillKind::kPill26);
    ui::TextStyle pfs = ui4::style(ui::Font::kB, 11.0f * s, ui4::kOffWhite, ui::Align::kCenter);
    ui4::fit_text(specs.perf, pfs, rw - 18.0f * s, 8.5f * s);
    list.text(0.5f * (rx0 + rx1), ry + 6.0f * s, specs.perf, pfs);

    // 2. Bottom +3.0 deg Group_BLADE@(640,522): TitleTapeText @(88.7, 435.7) + Horizontal Part/Tuning Blade!
    const std::string cat_title = std::string(shop_category_name(shop.category()));
    ui4::title_tape(list, s, 88.0f * s, 424.0f * s, cat_title, ui4::kBrushBlack, 34.0f, 540.0f);

    const float blade_y = 480.0f * s;
    const float blade_h = 138.0f * s;
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
        if (!sl.unlocked && !sl.unlock_hint.empty()) {
          ui::TextStyle hs = ui4::style(ui::Font::kB, 9.5f * s, ui4::kAmber, ui::Align::kCenter);
          ui4::fit_text(sl.unlock_hint, hs, cw - 20.0f * s, 7.5f * s);
          list.text(bx + 0.5f * cw, blade_y + 114.0f * s - lift, sl.unlock_hint, hs);
        }
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
      const auto rows = shop.visible_rows(eff_disc);
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
        const float ic_sz = 56.0f * s;
        const float ic_x0 = bx + 0.5f * (cw - ic_sz);
        const float ic_y0 = blade_y + 8.0f * s - lift;
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

        // Line 1: Slot name (full card width)
        ui::TextStyle ls = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite);
        ui4::fit_text(r.label, ls, cw - 16.0f * s, 9.5f * s);
        list.text(bx + 8.0f * s, blade_y + 68.0f * s - lift, r.label, ls);

        // Line 2: Localized part name on its own dedicated full-width row (zero collision with price!)
        ui::TextStyle ps = ui4::style(ui::Font::kB, 11.5f * s, act ? ui4::kOffWhite : ui4::kDim, ui::Align::kLeft, true);
        const float part_max_w = std::max(40.0f * s, cw - 16.0f * s);
        ui4::fit_text(r.part_label, ps, part_max_w, 9.0f * s);
        std::string part_txt = r.part_label;
        if (ui::List::measure(part_txt, ps) > part_max_w && part_txt.size() > 4) {
          while (part_txt.size() > 3 && ui::List::measure(part_txt + "...", ps) > part_max_w) part_txt.pop_back();
          part_txt += "...";
        }
        list.text(bx + 8.0f * s, blade_y + 87.0f * s - lift, part_txt, ps);

        // Line 3: Subtle separator + dedicated bottom-right Price / Status badge row
        list.rect(bx + 8.0f * s, blade_y + 106.0f * s - lift, bx + cw - 8.0f * s, blade_y + 107.0f * s - lift,
                  ui::rgba(255, 255, 255, act ? 56 : 28));
        const uint32_t scol = r.fitted ? ui4::kGreen : (r.owned ? ui4::kAmber : ui4::kYellow);
        ui::TextStyle ss = ui4::style(ui::Font::kE, 12.5f * s, scol, ui::Align::kRight, true);
        ui4::fit_text(r.state, ss, cw - 20.0f * s, 9.0f * s);
        list.text(bx + cw - 10.0f * s, blade_y + 112.0f * s - lift, r.state, ss);
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
                     const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice,
                     bool metric_units) {
  const float s = h / 720.0f;
  ui4::subtle_vignette(list, w, h, 0.20f);

  const float tilt_rad = scene_tilt_rad("106_C_CAR_LIST", "RotationHelper");
  const bool in_bay = browser.active_tab() == GarageTab::kRestorationBay;
  const int ready_bay_n = browser.ready_restoration_count();

  // Top-left PlayerBar + Dak's Garage Tab Strip (MY CARS / RESTORATION BAY)
  draw_hub_player_bar(list, s, credits,
                      in_bay ? ("DAK'S RESTORATION BAY  //  " + std::to_string(browser.restorations().size()) + " BAY")
                             : ("MY GARAGE  //  " + std::to_string(browser.visible().size()) + " CARS"));

  const std::string filt = browser.class_filter() < 0
                               ? "ALL CLASSES"
                               : (browser.class_filter() < int(bands.size())
                                      ? ("CLASS " + bands[size_t(browser.class_filter())].name)
                                      : "FILTERED");
  {
    ui4::TiltScope top_rot(list, 310.0f * s, 78.0f * s, tilt_rad);
    const float pill_y = 64.0f * s;
    // Tab pills (clickable via kHitTab 0 = MY CARS, 1 = RESTORATION BAY)
    list.add_hit(56.0f * s, pill_y, 174.0f * s, pill_y + 26.0f * s, ui::kHitTab, 0);
    ui4::pill(list, 56.0f * s, pill_y, 174.0f * s, pill_y + 26.0f * s,
              !in_bay ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
    list.text(115.0f * s, pill_y + 6.0f * s, "MY CARS",
              ui4::style(ui::Font::kE, 12.0f * s, ui4::kWhite, ui::Align::kCenter));

    std::string bay_lbl = "RESTORATION BAY";
    if (ready_bay_n > 0) bay_lbl += " (" + std::to_string(ready_bay_n) + " READY)";
    else if (!browser.restorations().empty()) bay_lbl += " (" + std::to_string(browser.restorations().size()) + ")";
    const float bay_x1 = ready_bay_n > 0 ? 396.0f * s : 352.0f * s;
    const uint32_t bay_col = in_bay ? ui4::kAmber : (ready_bay_n > 0 ? ui4::kGreen : ui4::kPillGrey);
    list.add_hit(180.0f * s, pill_y, bay_x1, pill_y + 26.0f * s, ui::kHitTab, 1);
    ui4::pill(list, 180.0f * s, pill_y, bay_x1, pill_y + 26.0f * s, bay_col, ui4::PillKind::kMenu, false, true);
    ui::TextStyle bay_ts =
        ui4::style(ui::Font::kE, 11.5f * s, (in_bay || ready_bay_n > 0) ? ui4::kInk : ui4::kWhite, ui::Align::kCenter);
    ui4::fit_text(bay_lbl, bay_ts, (bay_x1 - 180.0f * s) - 16.0f * s, 8.5f * s);
    list.text(0.5f * (180.0f * s + bay_x1), pill_y + 6.0f * s, bay_lbl, bay_ts);

    if (!in_bay) {
      const float flt_y = 95.0f * s;
      ui4::pill(list, 56.0f * s, flt_y, 232.0f * s, flt_y + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
      list.text(70.0f * s, flt_y + 5.0f * s, "SORT: " + std::string(garage_sort_name(browser.sort_mode())),
                ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
      ui4::pill(list, 238.0f * s, flt_y, 418.0f * s, flt_y + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
      list.text(252.0f * s, flt_y + 5.0f * s, "FILTER: " + filt, ui4::style(ui::Font::kB, 11.5f * s, ui4::kWhite));
      ui4::pill(list, 424.0f * s, flt_y, 580.0f * s, flt_y + 24.0f * s,
                browser.favorites_only() ? ui4::kPink : ui4::kPillGrey, ui4::PillKind::kMenu, false, true);
      list.text(438.0f * s, flt_y + 5.0f * s, browser.favorites_only() ? "FAVORITES: ONLY" : "FAVORITES: ALL",
                ui4::style(ui::Font::kB, 11.5f * s, browser.favorites_only() ? ui4::kYellow : ui4::kWhite));
    }
  }

  if (in_bay) {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);
    ui4::title_tape(list, s, 88.0f * s, 146.0f * s, "BARN FIND RESTORATION BAY", ui4::kAmber, 32.0f, 520.0f);

    const auto& bay = browser.restorations();
    if (bay.empty()) {
      const float px0 = 88.0f * s, py0 = 216.0f * s, px1 = w - 88.0f * s, py1 = 380.0f * s;
      ui4::panel(list, px0, py0, px1, py1, s, ui4::kCharcoalDark, ui4::kAmber, 0, true);
      list.text(0.5f * (px0 + px1), py0 + 52.0f * s, "NO BARN FINDS IN RESTORATION",
                ui4::style(ui::Font::kE, 22.0f * s, ui4::kWhite, ui::Align::kCenter));
      list.text(0.5f * (px0 + px1), py0 + 96.0f * s,
                "EXPLORE COLORADO TO DISCOVER BARN FIND RUMOURS — DAK WILL RESTORE THEM HERE.",
                ui4::style(ui::Font::kB, 13.5f * s, ui4::kDim, ui::Align::kCenter));
    } else {
      const float bx0 = 88.0f * s, by0 = 206.0f * s;
      const float card_w = std::min(560.0f * s, w * 0.48f);
      const float row_h = 84.0f * s, row_gap = 12.0f * s;
      const int sel = int(browser.selected_restoration_index());
      for (size_t i = 0; i < bay.size() && i < 5; ++i) {
        const auto& br = bay[i];
        const bool act = int(i) == sel;
        const float ry0 = by0 + float(i) * (row_h + row_gap);
        const float ry1 = ry0 + row_h;
        list.add_hit(bx0, ry0, bx0 + card_w, ry1, ui::kHitRow, int(i));
        draw_bigpane_card(list, bx0, ry0, bx0 + card_w, ry1, s, act, br.ready_to_claim ? ui4::kGreen : ui4::kAmber);

        ui::TextStyle nm = ui4::style(ui::Font::kE, 17.0f * s, ui4::kWhite, ui::Align::kLeft, true);
        ui4::fit_text(br.car_name, nm, card_w - 190.0f * s, 11.0f * s);
        list.text(bx0 + 16.0f * s, ry0 + 12.0f * s, br.car_name, nm);

        const int pct = int(std::lround(br.progress() * 100.0f));
        const std::string status_txt =
            br.ready_to_claim ? "READY TO CLAIM" : ("RESTORING... " + std::to_string(pct) + "%");
        const uint32_t badge_col = br.ready_to_claim ? ui4::kGreen : ui4::kAmber;
        ui4::brush(list, bx0 + card_w - 168.0f * s, ry0 + 10.0f * s, bx0 + card_w - 12.0f * s, ry0 + 34.0f * s,
                   badge_col);
        list.text(bx0 + card_w - 90.0f * s, ry0 + 15.0f * s, status_txt,
                  ui4::style(ui::Font::kE, 11.5f * s, ui4::kInk, ui::Align::kCenter));

        // Progress bar
        const float bar_x0 = bx0 + 16.0f * s, bar_x1 = bx0 + card_w - 16.0f * s;
        const float bar_y0 = ry0 + 46.0f * s, bar_y1 = ry0 + 56.0f * s;
        list.rect(bar_x0, bar_y0, bar_x1, bar_y1, ui::rgba(255, 255, 255, 32));
        list.rect(bar_x0, bar_y0, bar_x0 + (bar_x1 - bar_x0) * br.progress(), bar_y1, badge_col);

        const std::string sub = br.ready_to_claim
                                    ? "RESTORATION COMPLETE  •  PRESS [A] / ENTER TO CLAIM INTO YOUR GARAGE"
                                    : ("DAK'S WORKSHOP  •  " + std::to_string(int(std::ceil(br.remaining_s()))) +
                                       "s REMAINING");
        list.text(bx0 + 16.0f * s, ry0 + 62.0f * s, sub, ui4::style(ui::Font::kB, 11.0f * s, ui4::kDim));
      }

      if (const BarnRestorationEntry* cur = browser.selected_restoration()) {
        const float dx0 = bx0 + card_w + 28.0f * s, dx1 = w - 68.0f * s;
        const float dy0 = by0, dy1 = by0 + 250.0f * s;
        if (dx1 - dx0 > 180.0f * s) {
          ui4::panel(list, dx0, dy0, dx1, dy1, s, ui4::kCharcoalDark,
                     cur->ready_to_claim ? ui4::kGreen : ui4::kAmber, 0, true);
          list.text(dx0 + 18.0f * s, dy0 + 16.0f * s, "DAK'S RESTORATION REPORT",
                    ui4::style(ui::Font::kD, 13.0f * s, ui4::kDim));
          ui::TextStyle cn = ui4::style(ui::Font::kE, 21.0f * s, ui4::kWhite, ui::Align::kLeft, true);
          ui4::fit_text(cur->car_name, cn, dx1 - dx0 - 36.0f * s, 12.0f * s);
          list.text(dx0 + 18.0f * s, dy0 + 44.0f * s, cur->car_name, cn);
          const int pct = int(std::lround(cur->progress() * 100.0f));
          list.text(dx0 + 18.0f * s, dy0 + 90.0f * s,
                    cur->ready_to_claim ? "STATUS: READY TO CLAIM" : ("STATUS: RESTORING... " + std::to_string(pct) + "%"),
                    ui4::style(ui::Font::kE, 14.5f * s, cur->ready_to_claim ? ui4::kGreen : ui4::kAmber));
          list.text(dx0 + 18.0f * s, dy0 + 126.0f * s, "BARN FIND PRIZE CAR  •  REMOVAL VALUE: 0 CR",
                    ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
        }
      }
    }

    if (!notice.empty()) {
      ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
      ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
      ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
      list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
    }

    const bool can_claim = browser.selected_restoration() && browser.selected_restoration()->ready_to_claim;
    ui4::button_bar(list,
                    {{"A", can_claim ? "CLAIM CAR" : "RESTORING..."},
                     {"LB", "MY CARS"},
                     {"B", "BACK"}},
                    w, h, 56.0f);
    return;
  }

  // Entire Garage details & car carousel tilted +3.0 deg (106_C_CAR_LIST + 104_C_CAR_DETAILS)!
  {
    ui4::TiltScope rot(list, 0.5f * w, 0.5f * h, tilt_rad);

    // 1. Right-hand 104_C_CAR_DETAILS floating pill card (#BFBFBF PILL01 header + 5 stat bars + real specs)
    if (const GarageEntry* sel_e = browser.selected_entry()) {
      const float rx1 = w - 56.0f * s;
      const float rw = 408.0f * s;
      const float rx0 = rx1 - rw;
      float cy = 28.0f * s;
      const std::string cname =
          (bands.empty() || sel_e->class_id >= int(bands.size())) ? "?" : bands[size_t(sel_e->class_id)].name;

      // #BFBFBF PILL01 Header + PILLCLIPON01
      ui4::pill(list, rx0, cy, rx1, cy + 28.0f * s, ui::rgba(0xBF, 0xBF, 0xBF, 242), ui4::PillKind::kMenu, false, true);
      list.text(rx0 + 14.0f * s, cy + 6.5f * s, "CAR DETAILS", ui4::style(ui::Font::kD, 14.0f * s, ui4::kInk));
      float badge_r = rx1 - 6.0f * s;
      if (sel_e->is_current) {
        ui4::brush(list, badge_r - 110.0f * s, cy + 3.0f * s, badge_r, cy + 25.0f * s, ui4::kPink);
        list.text(badge_r - 55.0f * s, cy + 7.5f * s, "CURRENT CAR",
                  ui4::style(ui::Font::kE, 10.5f * s, ui4::kWhite, ui::Align::kCenter));
        badge_r -= 114.0f * s;
      }
      if (sel_e->favorite) {
        ui4::brush(list, badge_r - 58.0f * s, cy + 3.0f * s, badge_r, cy + 25.0f * s, ui4::kYellow);
        list.text(badge_r - 29.0f * s, cy + 7.5f * s, "FAV",
                  ui4::style(ui::Font::kE, 10.5f * s, ui4::kInk, ui::Align::kCenter));
        badge_r -= 62.0f * s;
      }
      if (sel_e->is_prize) {
        ui4::brush(list, badge_r - 62.0f * s, cy + 3.0f * s, badge_r, cy + 25.0f * s, ui4::kAmber);
        list.text(badge_r - 31.0f * s, cy + 7.5f * s, "PRIZE",
                  ui4::style(ui::Font::kE, 10.5f * s, ui4::kInk, ui::Align::kCenter));
      }
      cy += 31.0f * s;

      // Car Name, Make, Year & Class/PI row
      ui4::pill(list, rx0, cy, rx1, cy + 50.0f * s, ui4::kCharcoalDark, ui4::PillKind::kPillBig);
      ui4::manufacturer_logo(list, rx1 - 70.0f * s, cy + 5.0f * s, rx1 - 10.0f * s, cy + 45.0f * s, sel_e->make_name);
      list.text(rx0 + 14.0f * s, cy + 6.0f * s,
                std::to_string(sel_e->year) + "  " + sel_e->make_name + "  •  " + drive_label(sel_e->drive_type_id),
                ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
      std::string sel_label = car_name(sel_e->media_name);
      if (sel_label.empty() || sel_label.rfind("_&", 0) == 0) sel_label = sel_e->display_name;
      ui::TextStyle nm = ui4::style(ui::Font::kE, 18.0f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(sel_label, nm, rw - 90.0f * s, 11.0f * s);
      list.text(rx0 + 14.0f * s, cy + 23.0f * s, sel_label, nm);
      cy += 53.0f * s;

      // Class & PI + Garage/Sell Value row
      ui4::pill(list, rx0, cy, rx1, cy + 28.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
      ui4::class_badge(list, rx0 + 10.0f * s, cy + 3.0f * s, 22.0f * s, cname, sel_e->shown_pi, sel_e->class_id);
      const std::string sell_lbl =
          (sel_e->is_prize ? "SELL (PRIZE): " : "SELL (50%): ") + ui4::format_credits(sel_e->sell_cr);
      list.text(rx1 - 12.0f * s, cy + 7.0f * s, sell_lbl,
                ui4::style(ui::Font::kB, 11.5f * s, ui4::kYellow, ui::Align::kRight, true));
      cy += 31.0f * s;

      // 5 Floating PILL02 0..10 Stat Bars (Speed, Handling, Accel, Launch, Braking)
      const std::pair<const char*, double> gstats[5] = {
          {"SPEED", sel_e->speed_rating},
          {"HANDLING", sel_e->handling_rating},
          {"ACCEL", sel_e->accel_rating},
          {"LAUNCH", sel_e->launch_rating},
          {"BRAKING", sel_e->braking_rating},
      };
      for (int si = 0; si < 5; ++si) {
        ui4::pill(list, rx0, cy, rx1, cy + 23.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
        ui4::stat_bar(list, rx0 + 12.0f * s, cy + 1.5f * s, rw - 24.0f * s, 20.0f * s, gstats[si].first,
                      gstats[si].second, gstats[si].second, false, s);
        cy += 25.5f * s;
      }
      cy += 2.0f * s;

      // Real Specs Readout: Power, Torque, Weight, Displacement, Aspiration
      auto spec_row = [&](std::string_view k, std::string_view v, uint32_t vcol) {
        ui4::pill(list, rx0, cy, rx1, cy + 22.0f * s, ui4::kPillGrey, ui4::PillKind::kPill02);
        list.text(rx0 + 12.0f * s, cy + 5.0f * s, k, ui4::style(ui::Font::kD, 11.5f * s, ui4::kDim));
        ui::TextStyle vs = ui4::style(ui::Font::kB, 12.0f * s, vcol, ui::Align::kRight, true);
        ui4::fit_text(v, vs, rw - 135.0f * s, 8.5f * s);
        list.text(rx1 - 12.0f * s, cy + 5.0f * s, v, vs);
        cy += 24.5f * s;
      };
      char pwr_buf[64], wt_buf[64], disp_buf[48];
      if (metric_units) {
        std::snprintf(pwr_buf, sizeof pwr_buf, "%d kW  •  %d NM", int(std::lround(sel_e->peak_power_kw)),
                      int(std::lround(sel_e->peak_torque_nm)));
        std::snprintf(wt_buf, sizeof wt_buf, "%d KG", int(std::lround(sel_e->curb_weight_kg)));
      } else {
        std::snprintf(pwr_buf, sizeof pwr_buf, "%d HP  •  %d LB-FT",
                      int(std::lround(sel_e->peak_power_kw * 1.34102)),
                      int(std::lround(sel_e->peak_torque_nm * 0.737562)));
        std::snprintf(wt_buf, sizeof wt_buf, "%d LBS", int(std::lround(sel_e->curb_weight_kg * 2.20462)));
      }
      if (sel_e->displacement_cc >= 100.0) {
        std::snprintf(disp_buf, sizeof disp_buf, "%.1f L (%d CC)", sel_e->displacement_cc / 1000.0,
                      int(std::lround(sel_e->displacement_cc)));
      } else {
        std::snprintf(disp_buf, sizeof disp_buf, "N/A");
      }
      spec_row("POWER / TORQUE", pwr_buf, ui4::kWhite);
      spec_row("WEIGHT", wt_buf, ui4::kWhite);
      spec_row("DISPLACEMENT", disp_buf, ui4::kWhite);
      spec_row("ASPIRATION", sel_e->aspiration.empty() ? "Naturally Aspirated" : sel_e->aspiration, ui4::kOffWhite);
      spec_row("GARAGE VALUE", ui4::format_credits(sel_e->value_cr), ui4::kWhite);

      // Authentic vertical SCROLLINDICATOR beside details card
      ui4::scroll_indicator(list, rx1 + 6.0f * s, 30.0f * s, rx1 + 14.0f * s, cy - 2.0f * s,
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
        } else if (e.favorite) {
          ui4::brush(list, bx + cw - 52.0f * s, bot_y + 7.0f * s - lift, bx + cw - 6.0f * s, bot_y + 25.0f * s - lift,
                     ui4::kYellow);
          list.text(bx + cw - 29.0f * s, bot_y + 11.0f * s - lift, "FAV",
                    ui4::style(ui::Font::kE, 10.0f * s, ui4::kInk, ui::Align::kCenter));
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

  if (const GarageEntry* sel_e = browser.selected_entry();
      sel_e && browser.remove_confirm_garage_id() == sel_e->garage_id) {
    const float mw = 520.0f * s, mh = 92.0f * s;
    const float mx0 = 0.5f * (w - mw), my0 = 0.5f * (h - mh) - 20.0f * s;
    ui4::panel(list, mx0, my0, mx0 + mw, my0 + mh, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    list.text(mx0 + 0.5f * mw, my0 + 18.0f * s, "REMOVE CAR FROM GARAGE?",
              ui4::style(ui::Font::kE, 18.0f * s, ui4::kWhite, ui::Align::kCenter));
    const std::string sub = sel_e->is_prize
                                ? (sel_e->display_name + "  •  PRIZE / BARN CAR (0 CR)")
                                : (sel_e->display_name + "  •  PERMANENT REMOVAL (0 CR)");
    ui::TextStyle ss = ui4::style(ui::Font::kB, 12.5f * s, ui4::kAmber, ui::Align::kCenter);
    ui4::fit_text(sub, ss, mw - 28.0f * s, 9.0f * s);
    list.text(mx0 + 0.5f * mw, my0 + 52.0f * s, sub, ss);
  }

  if (!notice.empty()) {
    ui4::brush(list, w - 420.0f * s, h - 68.0f * s, w - 56.0f * s, h - 38.0f * s, ui4::kPink);
    ui::TextStyle ns = ui4::style(ui::Font::kB, 13.0f * s, ui4::kWhite, ui::Align::kRight);
    ui4::fit_text(notice, ns, 340.0f * s, 9.0f * s);
    list.text(w - 68.0f * s, h - 60.0f * s, notice, ns);
  }

  ui4::button_bar(list,
                  {{"A", "GET IN CAR"},
                   {"Y", "SORT"},
                   {"X", "FILTER"},
                   {"RB", "FAVORITE"},
                   {"LB", "REMOVE CAR"},
                   {"B", "BACK"}},
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
