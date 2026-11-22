#include "camera_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace carview {
namespace {

std::string fmt_meters(double m) {
  if (std::fabs(m) < 1e-4) return "DEFAULT";
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%+.2f M", m);
  return buf;
}

std::string fmt_pct(double scale, bool show_default = false) {
  const int p = int(std::lround(scale * 100.0));
  if (show_default && p == 100) return "DEFAULT";
  return std::to_string(p) + "%";
}

}  // namespace

void CameraEditorOverlay::open(const drive::CameraEditorConfig& ed, const drive::ImmersiveCameraConfig& imm,
                               const drive::HeadTrackingConfig& ht) {
  editor_ = ed;
  immersive_ = imm;
  head_ = ht;
  tab_ = Tab::kCameraEditor;
  row_ = 0;
  status_.clear();
  open_ = true;
}

void CameraEditorOverlay::switch_tab(int by) {
  const int n = int(Tab::kCount);
  tab_ = Tab(((int(tab_) + by) % n + n) % n);
  row_ = 0;
}

void CameraEditorOverlay::set_tab(Tab t) {
  tab_ = t;
  row_ = 0;
}

int CameraEditorOverlay::row_count() const {
  switch (tab_) {
    case Tab::kCameraEditor: return 6;
    case Tab::kImmersive: return 6;
    case Tab::kHeadTracking: return 6;
    default: return 1;
  }
}

void CameraEditorOverlay::move(int by) {
  const int n = row_count();
  row_ = ((row_ + by) % n + n) % n;
}

void CameraEditorOverlay::step(int by) {
  status_.clear();
  if (tab_ == Tab::kCameraEditor) {
    switch (row_) {
      case 0:
        editor_.chase_height_m = std::clamp(std::round((editor_.chase_height_m + 0.25 * by) * 4.0) / 4.0, -0.50, 1.50);
        break;
      case 1:
        editor_.chase_distance_m =
            std::clamp(std::round((editor_.chase_distance_m + 0.25 * by) * 4.0) / 4.0, -1.00, 3.00);
        break;
      case 2:
        editor_.chase_fov_scale =
            std::clamp(std::round((editor_.chase_fov_scale + 0.05 * by) * 20.0) / 20.0, 0.80, 1.30);
        break;
      case 3:
        editor_.chase_stiffness =
            std::clamp(std::round((editor_.chase_stiffness + 0.25 * by) * 4.0) / 4.0, 0.50, 2.00);
        break;
      case 4: {
        const int v = ((int(editor_.driving_view) + by) % 3 + 3) % 3;
        editor_.driving_view = drive::DrivingView(v);
        break;
      }
      default: break;
    }
    return;
  }

  if (tab_ == Tab::kImmersive) {
    switch (row_) {
      case 0: {
        const auto cur = immersive_.detect_preset();
        const int base = cur == drive::ImmersivePreset::kCustom ? 2 : int(cur);
        const int next = ((base + by) % 4 + 4) % 4;
        immersive_.apply_preset(drive::ImmersivePreset(next));
        break;
      }
      case 1:
        immersive_.gforce = std::clamp(std::round((immersive_.gforce + 0.25 * by) * 4.0) / 4.0, 0.0, 2.0);
        break;
      case 2:
        immersive_.lean = std::clamp(std::round((immersive_.lean + 0.25 * by) * 4.0) / 4.0, 0.0, 2.0);
        break;
      case 3:
        immersive_.shake = std::clamp(std::round((immersive_.shake + 0.25 * by) * 4.0) / 4.0, 0.0, 2.0);
        break;
      case 4:
        immersive_.head = std::clamp(std::round((immersive_.head + 0.25 * by) * 4.0) / 4.0, 0.0, 2.0);
        break;
      default: break;
    }
    return;
  }

  if (tab_ == Tab::kHeadTracking) {
    switch (row_) {
      case 0: head_.enabled = !head_.enabled; break;
      case 1: {
        static constexpr int kPorts[] = {4242, 4243, 5555, 5000, 4240};
        int idx = 0;
        for (int i = 0; i < 5; ++i)
          if (kPorts[i] == head_.port) idx = i;
        idx = ((idx + by) % 5 + 5) % 5;
        head_.port = kPorts[idx];
        break;
      }
      case 2: head_.invert[0] = !head_.invert[0]; break;
      case 3: head_.invert[1] = !head_.invert[1]; break;
      case 4: head_.invert[2] = !head_.invert[2]; break;
      default: break;
    }
  }
}

bool CameraEditorOverlay::activate() {
  if (tab_ == Tab::kCameraEditor && row_ == 5) {
    editor_.reset();
    status_ = "CAMERA EDITOR RESET TO DEFAULTS";
    return true;
  }
  if (tab_ == Tab::kImmersive && row_ == 5) {
    immersive_.apply_preset(drive::ImmersivePreset::kDefault);
    status_ = "IMMERSIVE CAMERA RESTORED TO DEFAULT";
    return true;
  }
  if (tab_ == Tab::kHeadTracking && row_ == 5) {
    head_ = drive::HeadTrackingConfig{};
    status_ = "HEAD TRACKING RESET (UDP 4242)";
    return true;
  }
  step(+1);
  return false;
}

std::vector<CameraEditorOverlay::RowView> CameraEditorOverlay::rows() const {
  std::vector<RowView> out;
  if (tab_ == Tab::kCameraEditor) {
    out.push_back({"CHASE HEIGHT", fmt_meters(editor_.chase_height_m),
                   "Vertical offset in metres applied live to both FollowLow and FollowHigh chase views.",
                   (editor_.chase_height_m + 0.50) / 2.00, false, row_ == 0});
    out.push_back({"CHASE DISTANCE", fmt_meters(editor_.chase_distance_m),
                   "Radius offset in metres behind the car relative to each car's camera data.",
                   (editor_.chase_distance_m + 1.00) / 4.00, false, row_ == 1});
    out.push_back({"CHASE FIELD OF VIEW", fmt_pct(editor_.chase_fov_scale, true),
                   "Chase camera lens FOV scale (80% to 130%) preserving depth perception.",
                   (editor_.chase_fov_scale - 0.80) / 0.50, false, row_ == 2});
    out.push_back({"CHASE STIFFNESS", fmt_pct(editor_.chase_stiffness, true),
                   "Yaw and height spring stiffness (scales K by S and D by sqrt(S) to preserve damping).",
                   (editor_.chase_stiffness - 0.50) / 1.50, false, row_ == 3});
    const char* dv = editor_.driving_view == drive::DrivingView::kWheel   ? "WHEEL"
                     : editor_.driving_view == drive::DrivingView::kReplay ? "REPLAY"
                                                                           : "NORMAL";
    out.push_back({"DRIVING VIEW", dv,
                   "Switch between Normal player cameras, Wheel arch camera, or car-relative Replay view.",
                   -1.0, false, row_ == 4});
    out.push_back({"RESET CAMERA EDITOR", "PRESS [A]",
                   "Restores chase height, distance, FOV, stiffness and Normal driving view.",
                   -1.0, true, row_ == 5});
    return out;
  }

  if (tab_ == Tab::kImmersive) {
    const auto p = immersive_.detect_preset();
    const char* pname = p == drive::ImmersivePreset::kOff      ? "OFF"
                        : p == drive::ImmersivePreset::kSubtle ? "SUBTLE"
                        : p == drive::ImmersivePreset::kDefault ? "DEFAULT"
                        : p == drive::ImmersivePreset::kStrong ? "STRONG"
                                                               : "CUSTOM";
    out.push_back({"IMMERSIVE PRESET", pname,
                   "Full effect in cockpit, 70% in hood, G-forces only in chase and bumper views.",
                   -1.0, false, row_ == 0});
    out.push_back({"CAMERA G-FORCE", fmt_pct(immersive_.gforce),
                   "Push back under acceleration, throw forward under braking, plus throttle/brake jolts.",
                   immersive_.gforce / 2.0, false, row_ == 1});
    out.push_back({"CAMERA LEAN", fmt_pct(immersive_.lean),
                   "Corner roll and subtle look-into-apex yaw in cockpit and hood views.",
                   immersive_.lean / 2.0, false, row_ == 2});
    out.push_back({"CAMERA SPEED SHAKE", fmt_pct(immersive_.shake),
                   "High-speed micro-vibration and hard-braking judder in cockpit and hood views.",
                   immersive_.shake / 2.0, false, row_ == 3});
    out.push_back({"CAMERA HEAD MOTION", fmt_pct(immersive_.head),
                   "Slow cockpit breathing and natural lateral head sway.",
                   immersive_.head / 2.0, false, row_ == 4});
    out.push_back({"RESTORE DEFAULT PRESET", "PRESS [A]",
                   "Sets all four Immersive Camera layers to 100% (DEFAULT).",
                   -1.0, true, row_ == 5});
    return out;
  }

  out.push_back({"OPENTRACK HEAD TRACKING", head_.enabled ? "ON" : "OFF",
                 "6-DoF UDP head tracking driving cockpit and hood views (1 s timeout smooth recenter).",
                 -1.0, false, row_ == 0});
  out.push_back({"OPENTRACK UDP PORT", std::to_string(head_.port),
                 "Localhost 127.0.0.1 UDP port matching OpenTrack's 'UDP over network' output.",
                 -1.0, false, row_ == 1});
  out.push_back({"REVERSE YAW", head_.invert[0] ? "YES" : "NO",
                 "Inverts left/right head turn if your tracker reports yaw opposite.",
                 -1.0, false, row_ == 2});
  out.push_back({"REVERSE PITCH", head_.invert[1] ? "YES" : "NO",
                 "Inverts up/down head pitch.",
                 -1.0, false, row_ == 3});
  out.push_back({"REVERSE ROLL", head_.invert[2] ? "YES" : "NO",
                 "Inverts head tilt roll.",
                 -1.0, false, row_ == 4});
  out.push_back({"RESET HEAD TRACKING", "PRESS [A]",
                 "Turns head tracking off, resets port to 4242 and clears axis inversions.",
                 -1.0, true, row_ == 5});
  return out;
}

void draw_camera_editor_ui4(ui::List& l, float screen_w, float screen_h, const CameraEditorOverlay& overlay,
                            const drive::HeadPose& live_head_pose) {
  const float s = screen_h / 720.0f;
  ui4::subtle_vignette(l, screen_w, screen_h, 0.28f);

  ui4::title_banner(l, s, 60.0f * s, 26.0f * s, "CAMERA EDITOR", "F6  /  DISPLAY  /  LIVE CAMERA RIG", true,
                    580.0f);

  const std::vector<std::string> tabs = {"CAMERA EDITOR", "IMMERSIVE CAMERA", "HEAD TRACKING"};
  const int cur_tab = int(overlay.active_tab());
  ui4::tab_strip(l, s, 56.0f * s, 114.0f * s, screen_w - 56.0f * s, tabs, cur_tab, true, ui4::kPink);

  const float top_y = 158.0f * s;
  const float bot_y = screen_h - 86.0f * s;
  const float lx0 = 56.0f * s;
  const float lx1 = screen_w * 0.61f;
  const float rx0 = lx1 + 18.0f * s;
  const float rx1 = screen_w - 56.0f * s;

  // Left card: live camera parameters.
  ui4::panel(l, lx0, top_y, lx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, lx0 + 12.0f * s, top_y + 10.0f * s, lx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(lx0 + 22.0f * s, top_y + 15.0f * s, tabs[size_t(cur_tab)] + "  —  APPLIES LIVE",
         ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  const auto rows = overlay.rows();
  const float row_h = 56.0f * s;
  float ry = top_y + 46.0f * s;
  const CameraEditorOverlay::RowView* sel_row = nullptr;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    if (r.selected) sel_row = &r;
    if (r.selected) {
      ui4::brush(l, lx0 + 8.0f * s, ry + 2.0f * s, lx1 - 8.0f * s, ry + row_h - 4.0f * s,
                 r.is_action ? ui4::kAmber : ui4::kPink);
    } else {
      l.rect(lx0 + 12.0f * s, ry + 2.0f * s, lx1 - 12.0f * s, ry + row_h - 4.0f * s,
             (i % 2 == 0) ? ui4::kRowAlt : ui::rgba(28, 30, 36, 196));
    }

    ui::TextStyle ns = ui4::style(r.selected ? ui::Font::kE : ui::Font::kB, 15.0f * s,
                                  r.selected && r.is_action ? ui4::kInk : ui4::kWhite);
    l.text(lx0 + 24.0f * s, ry + 11.0f * s, r.label, ns);

    // Optional live slider bar under the label.
    if (r.norm_bar >= 0.0) {
      const float bx0 = lx0 + 24.0f * s;
      const float bx1 = lx1 - 210.0f * s;
      const float by0 = ry + 34.0f * s;
      const float bh = 7.0f * s;
      l.rect(bx0, by0, bx1, by0 + bh, ui::rgba(0, 0, 0, 120));
      const float fill = std::clamp(float(r.norm_bar), 0.0f, 1.0f);
      l.rect(bx0, by0, bx0 + (bx1 - bx0) * fill, by0 + bh, r.selected ? ui4::kYellow : ui4::kPink);
    }

    const float vx0 = lx1 - 188.0f * s;
    const float vx1 = lx1 - 22.0f * s;
    l.rect(vx0, ry + 10.0f * s, vx1, ry + row_h - 14.0f * s,
           r.selected ? ui::rgba(14, 16, 20, 185) : ui::rgba(16, 18, 22, 165));
    const std::string vstr = r.selected && !r.is_action ? ("< " + r.value + " >") : r.value;
    ui::TextStyle vs = ui4::style(ui::Font::kE, 14.0f * s,
                                  r.selected ? (r.is_action ? ui4::kAmber : ui4::kYellow) : ui4::kWhite,
                                  ui::Align::kCenter);
    ui4::fit_text(vstr, vs, (vx1 - vx0) - 10.0f * s, 9.5f * s);
    l.text(0.5f * (vx0 + vx1), ry + 18.0f * s, vstr, vs);
    ry += row_h;
  }

  // Right card: live summary & telemetry readout.
  ui4::panel(l, rx0, top_y, rx1, bot_y, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
  ui4::offwhite_strip(l, rx0 + 12.0f * s, top_y + 10.0f * s, rx1 - 12.0f * s, top_y + 34.0f * s, s, false);
  l.text(rx0 + 22.0f * s, top_y + 15.0f * s, "LIVE RIG SUMMARY", ui4::style(ui::Font::kE, 13.0f * s, ui4::kInk));

  if (sel_row) {
    ui4::brush(l, rx0 + 8.0f * s, top_y + 46.0f * s, rx1 - 8.0f * s, top_y + 82.0f * s, ui4::kPink);
    ui::TextStyle stt = ui4::style(ui::Font::kE, 16.0f * s, ui4::kWhite);
    ui4::fit_text(sel_row->label, stt, (rx1 - rx0) - 34.0f * s, 11.0f * s);
    l.text(rx0 + 20.0f * s, top_y + 54.0f * s, sel_row->label, stt);

    ui::TextStyle ds = ui4::style(ui::Font::kB, 12.5f * s, ui4::kWhite, ui::Align::kLeft, true);
    ui4::fit_text(sel_row->detail, ds, (rx1 - rx0) - 40.0f * s, 9.5f * s);
    l.text(rx0 + 20.0f * s, top_y + 98.0f * s, sel_row->detail, ds);
  }

  // Active state cards (Camera Editor + Immersive + OpenTrack).
  const auto& ed = overlay.editor_config();
  const auto& im = overlay.immersive_config();
  const auto& ht = overlay.head_tracking_config();
  float sy = top_y + 158.0f * s;
  auto stat_box = [&](const char* title, const std::string& l1, const std::string& l2, uint32_t accent) {
    l.rect(rx0 + 14.0f * s, sy, rx1 - 14.0f * s, sy + 76.0f * s, ui::rgba(255, 255, 255, 14));
    l.rect(rx0 + 14.0f * s, sy, rx0 + 18.0f * s, sy + 76.0f * s, accent);
    l.text(rx0 + 28.0f * s, sy + 8.0f * s, title, ui4::style(ui::Font::kE, 12.0f * s, accent));
    l.text(rx0 + 28.0f * s, sy + 28.0f * s, l1, ui4::style(ui::Font::kB, 12.0f * s, ui4::kWhite));
    l.text(rx0 + 28.0f * s, sy + 48.0f * s, l2, ui4::style(ui::Font::kB, 11.5f * s, ui4::kDim));
    sy += 86.0f * s;
  };

  stat_box("CHASE RIG",
           "H " + fmt_meters(ed.chase_height_m) + "  |  D " + fmt_meters(ed.chase_distance_m),
           "FOV " + fmt_pct(ed.chase_fov_scale) + "  |  STIFF " + fmt_pct(ed.chase_stiffness) + "  |  VIEW " +
               drive::driving_view_name(ed.driving_view),
           ui4::kPink);

  stat_box("IMMERSIVE CAMERA",
           std::string("PRESET: ") + drive::immersive_preset_name(im.detect_preset()),
           "G " + fmt_pct(im.gforce) + "  LEAN " + fmt_pct(im.lean) + "  SHAKE " + fmt_pct(im.shake) + "  HEAD " +
               fmt_pct(im.head),
           ui4::kAmber);

  char pose_buf[96];
  std::snprintf(pose_buf, sizeof(pose_buf), "YAW %+.1f  PITCH %+.1f  ROLL %+.1f DEG", live_head_pose.yaw_deg,
                live_head_pose.pitch_deg, live_head_pose.roll_deg);
  stat_box("OPENTRACK 6-DOF",
           std::string(ht.enabled ? "LISTENING 127.0.0.1:" : "OFF (PORT ") + std::to_string(ht.port) +
               (ht.enabled ? "" : ")"),
           pose_buf, ht.enabled ? ui4::kGreen : ui4::kDim);

  if (!overlay.status_message().empty()) {
    ui4::pill(l, rx0 + 14.0f * s, bot_y - 42.0f * s, rx1 - 14.0f * s, bot_y - 12.0f * s, ui4::kGreen);
    l.text(rx0 + 26.0f * s, bot_y - 34.0f * s, overlay.status_message(),
           ui4::style(ui::Font::kE, 12.5f * s, ui4::kInk));
  }

  ui4::button_bar(l,
                  {{"DPAD_LR", "ADJUST"},
                   {"LB", "TAB"},
                   {"A", "APPLY / RESET"},
                   {"B", "BACK"}},
                  screen_w, screen_h, 56.0f);
}

}  // namespace carview
