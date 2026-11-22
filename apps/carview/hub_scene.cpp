#include "hub_scene.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "game_hud.h"

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
  auto push_tri = [&](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c,
                      std::array<float, 3> col) {
    wt.positions.push_back(a);
    wt.positions.push_back(b);
    wt.positions.push_back(c);
    wt.group.push_back(add_group(col));
  };

  // 1. Outer studio floor disc (radius 18 m, 36 segments).
  constexpr int kSegs = 36;
  const float y_outer = floor_y - 0.025f;
  for (int i = 0; i < kSegs; ++i) {
    const float a0 = 2.0f * kPi * float(i) / float(kSegs);
    const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
    const float r0 = 3.25f, r1 = 18.0f;
    std::array<float, 3> p00{r0 * std::cos(a0), y_outer, r0 * std::sin(a0)};
    std::array<float, 3> p01{r0 * std::cos(a1), y_outer, r0 * std::sin(a1)};
    std::array<float, 3> p10{r1 * std::cos(a0), y_outer, r1 * std::sin(a0)};
    std::array<float, 3> p11{r1 * std::cos(a1), y_outer, r1 * std::sin(a1)};
    push_tri(p00, p10, p11, floor_col);
    push_tri(p00, p11, p01, floor_col);
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

  // 4. Studio cyclorama cylinder wall (radius 17.5 m, height 8 m) for clean horizon framing.
  for (int i = 0; i < kSegs; ++i) {
    const float a0 = 2.0f * kPi * float(i) / float(kSegs);
    const float a1 = 2.0f * kPi * float(i + 1) / float(kSegs);
    const float r = 17.5f;
    std::array<float, 3> b0{r * std::cos(a0), y_outer, r * std::sin(a0)};
    std::array<float, 3> b1{r * std::cos(a1), y_outer, r * std::sin(a1)};
    std::array<float, 3> t0{r * std::cos(a0), y_outer + 8.0f, r * std::sin(a0)};
    std::array<float, 3> t1{r * std::cos(a1), y_outer + 8.0f, r * std::sin(a1)};
    push_tri(b0, t0, t1, wall_col);
    push_tri(b0, t1, b1, wall_col);
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
  out.key_light[3] = 1.55f;

  out.fill_light[0] = -0.60f;
  out.fill_light[1] = 0.45f;
  out.fill_light[2] = 0.66f;
  out.fill_light[3] = 0.65f;

  out.options[0] = 1.25f;  // exposure
  out.options[1] = 1.15f;  // studio cubemap reflection intensity
  out.options[2] = 0.0f;   // livery debug mask off
  out.options[3] = 0.48f;  // metallic flake strength

  out.tint[0] = out.tint[1] = out.tint[2] = out.tint[3] = 1.0f;
  out.reflect_tint[0] = out.reflect_tint[1] = out.reflect_tint[2] = out.reflect_tint[3] = 1.0f;

  if (room == HubRoom::kWorkshop) {
    out.tint[0] = 1.05f;
    out.tint[1] = 0.98f;
    out.tint[2] = 0.90f;
    out.options[0] = 1.20f;
  } else if (room == HubRoom::kPaintShop) {
    out.options[0] = 1.32f;
    out.options[1] = 1.25f;
  } else if (room == HubRoom::kBarnFind) {
    out.tint[0] = 1.12f;
    out.tint[1] = 0.94f;
    out.tint[2] = 0.76f;
    out.options[0] = 1.10f;
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

// --- UI Overlays ---

void draw_autoshow_hud(ui::List& list, float w, float h, const Autoshow& autoshow,
                       const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice) {
  // Top header bar + Make Carousel.
  list.rect(0, 0, w, 92.0f, ui::rgba(10, 12, 18, 224), 0.0f, ui::rgba(14, 16, 24, 195));
  list.rect(0, 90.0f, w, 93.0f, ui::rgba(240, 88, 24, 240));

  ui::TextStyle title_st;
  title_st.size = 24.0f;
  title_st.color = ui::rgba(255, 255, 255);
  title_st.font = ui::Font::kE;
  title_st.slant = 0.14f;
  list.text(32.0f, 14.0f, autoshow.tutorial_mode() ? "HORIZON AUTOSHOW  //  CHOOSE YOUR FIRST CAR" : "HORIZON AUTOSHOW",
            title_st);

  ui::TextStyle cr_st;
  cr_st.size = 20.0f;
  cr_st.color = ui::rgba(255, 206, 56);
  cr_st.align = ui::Align::kRight;
  cr_st.font = ui::Font::kB;
  list.text(w - 32.0f, 16.0f, credits_text(credits), cr_st);

  // Make Carousel strip.
  const auto& makes = autoshow.makes();
  if (!makes.empty()) {
    const int n = int(makes.size());
    const int sel = int(autoshow.selected_make_index());
    const int vis_n = std::min(n, 7);
    const float chip_w = std::min(168.0f, (w - 80.0f) / float(vis_n) - 8.0f);
    const int first = std::clamp(sel - vis_n / 2, 0, std::max(0, n - vis_n));
    float cx = 32.0f;
    for (int i = first; i < first + vis_n; ++i) {
      const bool active = (i == sel);
      list.rect(cx, 50.0f, cx + chip_w, 82.0f, active ? ui::rgba(240, 88, 24, 245) : ui::rgba(28, 34, 46, 210), 4.0f);
      ui::TextStyle ms;
      ms.size = 13.0f;
      ms.color = active ? ui::rgba(255, 255, 255) : ui::rgba(195, 204, 218);
      ms.align = ui::Align::kCenter;
      ms.font = ui::Font::kB;
      std::string lbl = makes[size_t(i)].name;
      if (lbl.size() > 16) lbl = lbl.substr(0, 15) + ".";
      list.text(cx + chip_w * 0.5f, 59.0f, lbl, ms);
      cx += chip_w + 8.0f;
    }
  }

  // Right spec & 5-stat card.
  const float card_w = 344.0f;
  const float card_x = w - card_w - 28.0f;
  const float card_y = 112.0f;
  const float card_h = 372.0f;
  list.shadow(card_x, card_y, card_x + card_w, card_y + card_h, ui::rgba(0, 0, 0, 130), 8.0f, 14.0f);
  list.rect(card_x, card_y, card_x + card_w, card_y + card_h, ui::rgba(14, 17, 24, 224), 8.0f);
  list.frame(card_x, card_y, card_x + card_w, card_y + card_h, ui::rgba(255, 255, 255, 36), 1.0f, 8.0f);

  const AutoshowCar* sel_car = autoshow.selected_car();
  if (!sel_car) {
    ui::TextStyle es;
    es.size = 18.0f;
    es.color = ui::rgba(255, 160, 80);
    es.align = ui::Align::kCenter;
    list.text(card_x + card_w * 0.5f, card_y + 160.0f,
              autoshow.empty_reason().empty() ? "NO CARS AVAILABLE" : autoshow.empty_reason(), es);
  } else {
    const int cls = bands.empty() ? int(sel_car->dealer.class_id)
                                  : garage::class_of(sel_car->dealer.performance_index, bands);
    const std::string cls_name = (bands.empty() || cls >= int(bands.size())) ? "?" : bands[size_t(cls)].name;
    const int shown_pi = bands.empty() ? int(std::ceil(sel_car->dealer.performance_index * 999.0))
                                       : garage::shown_index(sel_car->dealer.performance_index, bands);

    ui::TextStyle mk_st;
    mk_st.size = 14.0f;
    mk_st.color = ui::rgba(240, 108, 42);
    mk_st.font = ui::Font::kB;
    list.text(card_x + 18.0f, card_y + 16.0f, std::to_string(sel_car->dealer.year) + "  " + sel_car->make_name, mk_st);

    ui::TextStyle nm_st;
    nm_st.size = 22.0f;
    nm_st.color = ui::rgba(255, 255, 255);
    nm_st.font = ui::Font::kE;
    list.text(card_x + 18.0f, card_y + 38.0f, car_name(sel_car->dealer.media_name), nm_st);

    draw_class_badge(list, card_x + 18.0f, card_y + 72.0f, 28.0f, cls, cls_name, shown_pi);

    ui::TextStyle pr_st;
    pr_st.size = 20.0f;
    pr_st.color = credits >= sel_car->dealer.base_cost ? ui::rgba(110, 238, 138) : ui::rgba(245, 92, 82);
    pr_st.align = ui::Align::kRight;
    pr_st.font = ui::Font::kB;
    list.text(card_x + card_w - 18.0f, card_y + 76.0f, credits_text(sel_car->dealer.base_cost), pr_st);

    // 5-Stat bars.
    float sy = card_y + 118.0f;
    draw_stat_bar(list, card_x + 18.0f, sy, card_w - 36.0f, 20.0f, "SPEED", sel_car->speed_rating, sel_car->speed_rating, false);
    sy += 26.0f;
    draw_stat_bar(list, card_x + 18.0f, sy, card_w - 36.0f, 20.0f, "HANDLING", sel_car->handling_rating, sel_car->handling_rating, false);
    sy += 26.0f;
    draw_stat_bar(list, card_x + 18.0f, sy, card_w - 36.0f, 20.0f, "ACCEL", sel_car->accel_rating, sel_car->accel_rating, false);
    sy += 26.0f;
    draw_stat_bar(list, card_x + 18.0f, sy, card_w - 36.0f, 20.0f, "LAUNCH", sel_car->launch_rating, sel_car->launch_rating, false);
    sy += 26.0f;
    draw_stat_bar(list, card_x + 18.0f, sy, card_w - 36.0f, 20.0f, "BRAKING", sel_car->braking_rating, sel_car->braking_rating, false);

    // Technical specs row.
    char spec[128];
    const int hp = int(std::lround(sel_car->peak_power_kw * 1.34102));
    std::snprintf(spec, sizeof spec, "%d HP   |   %.0f NM   |   %.0f KG   |   %s", hp, sel_car->peak_torque_nm,
                  sel_car->curb_weight_kg, drive_label(sel_car->drive_type_id));
    ui::TextStyle sp_st;
    sp_st.size = 13.0f;
    sp_st.color = ui::rgba(186, 194, 208);
    sp_st.align = ui::Align::kCenter;
    list.text(card_x + card_w * 0.5f, card_y + 260.0f, spec, sp_st);

    // Factory Colour Bar Picker inside card bottom.
    ui::TextStyle cl_st;
    cl_st.size = 13.0f;
    cl_st.color = ui::rgba(220, 226, 236);
    cl_st.font = ui::Font::kB;
    list.text(card_x + 18.0f, card_y + 292.0f,
              "MANUFACTURER COLOR  (" + std::to_string(autoshow.selected_colour_index() + 1) + "/" +
                  std::to_string(std::max<size_t>(1, sel_car->swatches.size())) + ")",
              cl_st);
    const size_t nsw = std::min<size_t>(sel_car->swatches.size(), 10);
    const float sw_w = 26.0f, sw_gap = 5.0f;
    float sx = card_x + 18.0f;
    for (size_t i = 0; i < nsw; ++i) {
      const auto& sw = sel_car->swatches[i];
      const uint8_t r = uint8_t((sw.rgb >> 16) & 0xFF);
      const uint8_t g = uint8_t((sw.rgb >> 8) & 0xFF);
      const uint8_t b = uint8_t(sw.rgb & 0xFF);
      const bool active = (i == autoshow.selected_colour_index());
      list.rect(sx, card_y + 318.0f, sx + sw_w, card_y + 346.0f, ui::rgba(r, g, b, 255), 3.0f);
      if (sw.metallic) {
        list.rect(sx + 2.0f, card_y + 320.0f, sx + sw_w - 2.0f, card_y + 327.0f, ui::rgba(255, 255, 255, 64), 2.0f);
      }
      if (active) {
        list.frame(sx - 2.0f, card_y + 316.0f, sx + sw_w + 2.0f, card_y + 348.0f, ui::rgba(255, 255, 255, 255), 2.0f, 4.0f);
      }
      sx += sw_w + sw_gap;
    }
  }

  // Bottom car strip + controls footer.
  const float bot_y = h - 98.0f;
  list.rect(0, bot_y, w, h, ui::rgba(10, 12, 18, 224));
  const auto& vis = autoshow.visible_cars();
  if (!vis.empty()) {
    const int n = int(vis.size());
    const int sel = int(autoshow.selected_index());
    const int count = std::min(n, 5);
    const int first = std::clamp(sel - count / 2, 0, std::max(0, n - count));
    const float cw = (w - 64.0f) / float(count) - 10.0f;
    float bx = 32.0f;
    for (int i = first; i < first + count; ++i) {
      const auto& c = vis[size_t(i)];
      const bool act = (i == sel);
      list.rect(bx, bot_y + 10.0f, bx + cw, bot_y + 62.0f, act ? ui::rgba(240, 88, 24, 235) : ui::rgba(26, 31, 42, 210),
                5.0f);
      ui::TextStyle cs;
      cs.size = 13.0f;
      cs.color = ui::rgba(255, 255, 255);
      cs.font = ui::Font::kB;
      list.text(bx + 10.0f, bot_y + 17.0f, car_name(c.dealer.media_name), cs);
      cs.size = 12.0f;
      cs.color = act ? ui::rgba(255, 244, 210) : ui::rgba(176, 184, 198);
      list.text(bx + 10.0f, bot_y + 38.0f, credits_text(c.dealer.base_cost), cs);
      bx += cw + 10.0f;
    }
  }
  ui::TextStyle hint;
  hint.size = 12.0f;
  hint.color = ui::rgba(182, 190, 204);
  list.text(32.0f, h - 24.0f,
            notice.empty() ? "[Q/E] MAKE  [UP/DOWN] CAR  [C] COLOR  [ENTER] BUY  [ESC] BACK" : notice, hint);
}

void draw_upgrade_shop_hud(ui::List& list, float w, float h, const UpgradeShop& shop,
                           const std::vector<garage::ClassBand>& bands, int discount_percent, int64_t credits,
                           const std::string& car_title, const std::string& notice) {
  // Top bar + 8 Category tabs.
  list.rect(0, 0, w, 92.0f, ui::rgba(12, 14, 19, 230));
  list.rect(0, 90.0f, w, 93.0f, ui::rgba(246, 152, 24, 240));

  ui::TextStyle tst;
  tst.size = 23.0f;
  tst.color = ui::rgba(255, 255, 255);
  tst.font = ui::Font::kE;
  tst.slant = 0.14f;
  list.text(28.0f, 14.0f, "DAK'S GARAGE  //  " + car_title, tst);

  ui::TextStyle cr;
  cr.size = 18.0f;
  cr.color = ui::rgba(255, 206, 56);
  cr.align = ui::Align::kRight;
  cr.font = ui::Font::kB;
  list.text(w - 28.0f, 16.0f,
            credits_text(credits) + (discount_percent > 0 ? ("  (" + std::to_string(discount_percent) + "% OFF)") : ""),
            cr);

  const int ncat = int(ShopCategory::kCount);
  const float tw = (w - 56.0f) / float(ncat) - 5.0f;
  float tx = 28.0f;
  for (int i = 0; i < ncat; ++i) {
    const bool act = (ShopCategory(i) == shop.category());
    list.rect(tx, 50.0f, tx + tw, 82.0f, act ? ui::rgba(246, 142, 24, 245) : ui::rgba(28, 32, 42, 210), 4.0f);
    ui::TextStyle cs;
    cs.size = 11.5f;
    cs.color = act ? ui::rgba(14, 16, 20) : ui::rgba(204, 210, 222);
    cs.align = ui::Align::kCenter;
    cs.font = ui::Font::kB;
    list.text(tx + tw * 0.5f, 60.0f, shop_category_name(ShopCategory(i)), cs);
    tx += tw + 5.0f;
  }

  // Left slots/parts/sliders panel.
  const float lx = 28.0f, ly = 110.0f, lw = 420.0f, lh = h - 166.0f;
  list.rect(lx, ly, lx + lw, ly + lh, ui::rgba(14, 17, 24, 224), 8.0f);
  list.frame(lx, ly, lx + lw, ly + lh, ui::rgba(255, 255, 255, 34), 1.0f, 8.0f);

  const auto rows = shop.visible_rows(discount_percent);
  const int n = int(rows.size());
  const int sel = n > 0 ? int(std::min(shop.selected_row(), rows.size() - 1)) : 0;
  const int max_vis = std::max(1, int((lh - 24.0f) / 48.0f));
  const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
  float ry = ly + 14.0f;
  for (int i = first; i < std::min(n, first + max_vis); ++i) {
    const auto& r = rows[size_t(i)];
    const bool active = (i == sel);
    list.rect(lx + 10.0f, ry, lx + lw - 10.0f, ry + 42.0f,
              active ? ui::rgba(246, 142, 24, 232) : ui::rgba(26, 31, 42, 195), 5.0f);
    ui::TextStyle ls;
    ls.size = 13.5f;
    ls.color = active ? ui::rgba(16, 18, 22) : ui::rgba(240, 244, 250);
    ls.font = ui::Font::kB;
    list.text(lx + 20.0f, ry + 6.0f, r.label, ls);

    ls.size = 12.0f;
    ls.color = active ? ui::rgba(32, 26, 18) : ui::rgba(172, 182, 198);
    list.text(lx + 20.0f, ry + 23.0f, r.part_label, ls);

    ls.align = ui::Align::kRight;
    ls.color = active ? ui::rgba(14, 16, 20)
                      : (r.fitted ? ui::rgba(80, 232, 130) : ui::rgba(255, 208, 72));
    list.text(lx + lw - 20.0f, ry + 13.0f, r.state, ls);
    ry += 48.0f;
  }

  // Right PI & 5-Stat Diff Preview card.
  const float rx = w - 360.0f, rtop = 110.0f, rw = 332.0f, rh = 334.0f;
  list.rect(rx, rtop, rx + rw, rtop + rh, ui::rgba(14, 17, 24, 226), 8.0f);
  list.frame(rx, rtop, rx + rw, rtop + rh, ui::rgba(255, 255, 255, 36), 1.0f, 8.0f);

  const auto& cur = shop.current_pi();
  const auto& prv = shop.preview_pi();
  const bool diff = shop.has_preview_pi();
  const std::string c_name =
      (bands.empty() || cur.class_id >= int(bands.size())) ? "?" : bands[size_t(cur.class_id)].name;
  const std::string p_name =
      (bands.empty() || prv.class_id >= int(bands.size())) ? "?" : bands[size_t(prv.class_id)].name;

  ui::TextStyle ph;
  ph.size = 13.0f;
  ph.color = ui::rgba(186, 194, 210);
  ph.font = ui::Font::kB;
  list.text(rx + 18.0f, rtop + 14.0f, "PERFORMANCE INDEX PREVIEW", ph);
  draw_class_badge(list, rx + 18.0f, rtop + 38.0f, 30.0f, cur.class_id, c_name, cur.shown_pi);

  if (diff && prv.shown_pi != cur.shown_pi) {
    ui::TextStyle arr = ph;
    arr.size = 18.0f;
    arr.color = prv.shown_pi > cur.shown_pi ? ui::rgba(72, 236, 126) : ui::rgba(245, 86, 76);
    list.text(rx + 136.0f, rtop + 42.0f, "->", arr);
    draw_class_badge(list, rx + 168.0f, rtop + 38.0f, 30.0f, prv.class_id, p_name, prv.shown_pi);
  }

  float sy = rtop + 88.0f;
  draw_stat_bar(list, rx + 18.0f, sy, rw - 36.0f, 20.0f, "SPEED", cur.stats.speed_rating, prv.stats.speed_rating, diff);
  sy += 26.0f;
  draw_stat_bar(list, rx + 18.0f, sy, rw - 36.0f, 20.0f, "HANDLING", cur.stats.handling_rating, prv.stats.handling_rating, diff);
  sy += 26.0f;
  draw_stat_bar(list, rx + 18.0f, sy, rw - 36.0f, 20.0f, "ACCEL", cur.stats.accel_rating, prv.stats.accel_rating, diff);
  sy += 26.0f;
  draw_stat_bar(list, rx + 18.0f, sy, rw - 36.0f, 20.0f, "LAUNCH", cur.stats.launch_rating, prv.stats.launch_rating, diff);
  sy += 26.0f;
  draw_stat_bar(list, rx + 18.0f, sy, rw - 36.0f, 20.0f, "BRAKING", cur.stats.braking_rating, prv.stats.braking_rating, diff);

  char specs[140];
  const int hp0 = int(std::lround(cur.stats.peak_power_kw * 1.34102));
  const int hp1 = int(std::lround(prv.stats.peak_power_kw * 1.34102));
  std::snprintf(specs, sizeof specs, "POWER %d->%d HP   |   WEIGHT %.0f->%.0f KG", hp0, hp1,
                cur.stats.curb_weight_kg, prv.stats.curb_weight_kg);
  ui::TextStyle sp;
  sp.size = 12.5f;
  sp.color = ui::rgba(202, 210, 224);
  sp.align = ui::Align::kCenter;
  list.text(rx + rw * 0.5f, rtop + 236.0f, specs, sp);

  char accel_str[140];
  std::snprintf(accel_str, sizeof accel_str, "0-60: %.2fs   |   60-0: %.1fm   |   LAT G: %.2fg",
                prv.stats.time_0_60_s, prv.stats.brake_60_0_m, prv.stats.lat_g_60);
  list.text(rx + rw * 0.5f, rtop + 260.0f, accel_str, sp);

  // Footer.
  list.rect(0, h - 38.0f, w, h, ui::rgba(10, 12, 18, 226));
  ui::TextStyle ft;
  ft.size = 12.5f;
  ft.color = ui::rgba(196, 204, 218);
  list.text(28.0f, h - 26.0f,
            notice.empty()
                ? "[Q/E] CATEGORY  [UP/DOWN] SLOT  [LEFT/RIGHT] PART/SLIDER  [ENTER] FIT  [X] SELL PART  [ESC] EXIT"
                : notice,
            ft);
}

void draw_garage_hud(ui::List& list, float w, float h, const GarageBrowser& browser,
                     const std::vector<garage::ClassBand>& bands, int64_t credits, const std::string& notice) {
  list.rect(0, 0, w, 78.0f, ui::rgba(10, 13, 20, 228));
  list.rect(0, 76.0f, w, 79.0f, ui::rgba(52, 168, 238, 240));

  ui::TextStyle ts;
  ts.size = 24.0f;
  ts.color = ui::rgba(255, 255, 255);
  ts.font = ui::Font::kE;
  ts.slant = 0.14f;
  list.text(28.0f, 16.0f, "MY GARAGE", ts);

  const std::string filt = browser.class_filter() < 0
                               ? "ALL CLASSES"
                               : (browser.class_filter() < int(bands.size())
                                      ? ("CLASS " + bands[size_t(browser.class_filter())].name)
                                      : "FILTERED");
  ui::TextStyle st;
  st.size = 14.0f;
  st.color = ui::rgba(196, 208, 226);
  st.font = ui::Font::kB;
  list.text(28.0f, 48.0f,
            "SORT: " + std::string(garage_sort_name(browser.sort_mode())) + "   |   FILTER: " + filt + "   |   " +
                std::to_string(browser.visible().size()) + " CARS",
            st);

  ui::TextStyle cr = st;
  cr.size = 19.0f;
  cr.color = ui::rgba(255, 206, 56);
  cr.align = ui::Align::kRight;
  list.text(w - 28.0f, 22.0f, credits_text(credits), cr);

  // Left car list.
  const float lx = 28.0f, ly = 96.0f, lw = 410.0f, lh = h - 150.0f;
  list.rect(lx, ly, lx + lw, ly + lh, ui::rgba(12, 16, 24, 222), 8.0f);
  const auto& vis = browser.visible();
  const int n = int(vis.size());
  const int sel = int(browser.selected_index());
  const int max_vis = std::max(1, int((lh - 24.0f) / 48.0f));
  const int first = std::clamp(sel - max_vis / 2, 0, std::max(0, n - max_vis));
  float ry = ly + 12.0f;
  for (int i = first; i < std::min(n, first + max_vis); ++i) {
    const auto& e = vis[size_t(i)];
    const bool act = (i == sel);
    list.rect(lx + 10.0f, ry, lx + lw - 10.0f, ry + 42.0f,
              act ? ui::rgba(52, 158, 232, 232) : ui::rgba(24, 30, 42, 195), 5.0f);
    const std::string cname =
        (bands.empty() || e.class_id >= int(bands.size())) ? "?" : bands[size_t(e.class_id)].name;
    draw_class_badge(list, lx + 16.0f, ry + 8.0f, 24.0f, e.class_id, cname, e.shown_pi);

    ui::TextStyle ns;
    ns.size = 13.5f;
    ns.color = ui::rgba(255, 255, 255);
    ns.font = ui::Font::kB;
    list.text(lx + 112.0f, ry + 6.0f, e.display_name, ns);
    ns.size = 11.5f;
    ns.color = act ? ui::rgba(232, 246, 255) : ui::rgba(170, 180, 196);
    list.text(lx + 112.0f, ry + 24.0f,
              std::string(drive_label(e.drive_type_id)) + "   |   SELL " + credits_text(e.sell_cr) +
                  (e.is_current ? "   [ACTIVE]" : ""),
              ns);
    ry += 48.0f;
  }

  list.rect(0, h - 38.0f, w, h, ui::rgba(10, 12, 18, 226));
  ui::TextStyle ft;
  ft.size = 12.5f;
  ft.color = ui::rgba(196, 204, 218);
  list.text(28.0f, h - 26.0f,
            notice.empty() ? "[UP/DOWN] SELECT  [ENTER] GET IN CAR  [S] SORT  [F] FILTER  [X] SELL CAR (50%)  [ESC] BACK"
                           : notice,
            ft);
}

void draw_barn_find_hud(ui::List& list, float w, float h, const BarnFindCard& card) {
  if (!card.visible) return;
  const float cw = 520.0f, ch = 220.0f;
  const float cx = (w - cw) * 0.5f, cy = h - ch - 48.0f;
  list.shadow(cx, cy, cx + cw, cy + ch, ui::rgba(0, 0, 0, 160), 10.0f, 18.0f);
  list.rect(cx, cy, cx + cw, cy + ch, ui::rgba(22, 17, 13, 238), 10.0f);
  list.frame(cx, cy, cx + cw, cy + ch, ui::rgba(232, 164, 52, 220), 2.0f, 10.0f);
  list.rect(cx, cy, cx + cw, cy + 44.0f, ui::rgba(196, 122, 26, 235), 10.0f);

  ui::TextStyle hdr;
  hdr.size = 19.0f;
  hdr.color = ui::rgba(255, 250, 238);
  hdr.font = ui::Font::kE;
  hdr.align = ui::Align::kCenter;
  list.text(cx + cw * 0.5f, cy + 12.0f, "BARN FIND RESTORATION  //  " + card.barn_name, hdr);

  ui::TextStyle nm;
  nm.size = 26.0f;
  nm.color = ui::rgba(255, 255, 255);
  nm.font = ui::Font::kE;
  nm.align = ui::Align::kCenter;
  list.text(cx + cw * 0.5f, cy + 64.0f,
            (card.year > 0 ? (std::to_string(card.year) + " ") : "") + card.car_name, nm);

  // Restoration progress bar.
  const float bx = cx + 48.0f, bw = cw - 96.0f, by = cy + 118.0f;
  list.rect(bx, by, bx + bw, by + 16.0f, ui::rgba(46, 36, 26, 220), 4.0f);
  list.rect(bx, by, bx + bw * std::clamp(card.restore_progress, 0.0f, 1.0f), by + 16.0f,
            ui::rgba(76, 218, 108, 245), 4.0f);

  ui::TextStyle sub;
  sub.size = 13.5f;
  sub.color = ui::rgba(236, 222, 198);
  sub.align = ui::Align::kCenter;
  sub.font = ui::Font::kB;
  list.text(cx + cw * 0.5f, cy + 152.0f, card.subtitle, sub);

  sub.size = 12.0f;
  sub.color = ui::rgba(196, 182, 160);
  list.text(cx + cw * 0.5f, cy + 184.0f, "[ENTER] CONTINUE TO GARAGE", sub);
}

}  // namespace carview
