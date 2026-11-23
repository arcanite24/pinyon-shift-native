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
  // Phase 4: compact +3.0 deg right-side 516_I_EFFECTSMODE-style slider column so the live 3D car
  // and camera remain completely unobstructed while tuning.
  const float s = screen_h / 720.0f;
  const std::vector<std::string> tabs = {"CAMERA", "IMMERSIVE", "TRACKING"};
  const int cur_tab = int(overlay.active_tab());
  const auto& ed = overlay.editor_config();
  const auto& im = overlay.immersive_config();
  const auto& ht = overlay.head_tracking_config();

  // Compact top-left TitleTape + slim live telemetry pill (leaves center 3D car 100% open).
  {
    ui4::title_banner(l, s, 42.0f * s, 24.0f * s, "CAMERA EDITOR", "LIVE CAMERA RIG", true, 320.0f);
    l.turn(192.0f * s, 130.0f * s, ui4::kTiltRad);
    // Compact live rig telemetry strip tucked under the title tape.
    const float sx0 = 42.0f * s, sy0 = 104.0f * s, sx1 = 352.0f * s, sy1 = 152.0f * s;
    ui4::pill(l, sx0, sy0, sx1, sy1, ui::rgba(33, 33, 33, 218), ui4::PillKind::kPill02, true);
    l.text(sx0 + 14.0f * s, sy0 + 7.0f * s,
           "VIEW " + std::string(drive::driving_view_name(ed.driving_view)) + "  •  " +
               std::string(drive::immersive_preset_name(im.detect_preset())),
           ui4::style(ui::Font::kE, 10.5f * s, ui4::kYellow));
    char rig_buf[128];
    if (cur_tab == 2) {
      std::snprintf(rig_buf, sizeof(rig_buf), "6-DOF %s  Y%+.1f P%+.1f R%+.1f", ht.enabled ? "ON" : "OFF",
                    live_head_pose.yaw_deg, live_head_pose.pitch_deg, live_head_pose.roll_deg);
    } else {
      std::snprintf(rig_buf, sizeof(rig_buf), "H %s  D %s  FOV %s  G %s", fmt_meters(ed.chase_height_m).c_str(),
                    fmt_meters(ed.chase_distance_m).c_str(), fmt_pct(ed.chase_fov_scale).c_str(),
                    fmt_pct(im.gforce).c_str());
    }
    ui::TextStyle rs = ui4::style(ui::Font::kB, 10.5f * s, ui4::kWhite);
    ui4::fit_text(rig_buf, rs, (sx1 - sx0) - 24.0f * s, 8.0f * s);
    l.text(sx0 + 14.0f * s, sy0 + 25.0f * s, rig_buf, rs);
    l.unturn();
  }

  // Right-side +3.0 deg 516_I_EFFECTSMODE-style floating slider column (x=880..1246).
  const float rx0 = screen_w - 400.0f * s;
  const float rx1 = screen_w - 34.0f * s;
  const float rw = rx1 - rx0;
  {
    l.turn(0.5f * (rx0 + rx1), 350.0f * s, ui4::kTiltRad);

    // Compact LB / RB tab strip across the top of the right drawer.
    ui4::tab_strip(l, s, rx0, 62.0f * s, rx1, tabs, cur_tab, true, ui4::kPink);

    // Header pill (PILL01 silver/grey like 516_I_EFFECTSMODE).
    const float hy0 = 104.0f * s, hy1 = 132.0f * s;
    ui4::pill(l, rx0, hy0, rx1, hy1, ui::rgba(191, 191, 191, 245), ui4::PillKind::kMenu, false, true);
    l.text(rx0 + 14.0f * s, hy0 + 7.0f * s, tabs[size_t(cur_tab)] + " RIG  •  APPLIES LIVE",
           ui4::style(ui::Font::kE, 11.5f * s, ui4::kInk));

    const auto rows = overlay.rows();
    const float row_h = 54.0f * s;
    const float gap = 5.0f * s;
    float ry = hy1 + 8.0f * s;
    const CameraEditorOverlay::RowView* sel_row = nullptr;

    for (size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      if (r.selected) sel_row = &r;
      const uint32_t row_bg = r.selected ? (r.is_action ? ui4::kAmber : ui4::kPink) : ui::rgba(33, 33, 33, 224);
      const uint32_t fg = (r.selected && r.is_action) ? ui4::kInk : ui4::kWhite;
      ui4::pill(l, rx0, ry, rx1, ry + row_h, row_bg, ui4::PillKind::kPill02, r.selected);

      // Top line: parameter name + right-aligned value with real OPT_BAR_ARROW glyphs.
      ui::TextStyle ns = ui4::style(r.selected ? ui::Font::kE : ui::Font::kB, 12.5f * s, fg);
      ui4::fit_text(r.label, ns, rw * 0.56f, 9.0f * s);
      l.text(rx0 + 14.0f * s, ry + 7.0f * s, r.label, ns);

      const float arrow_sz = 12.0f * s;
      const float val_right = rx1 - (r.selected && !r.is_action ? 24.0f * s : 14.0f * s);
      ui::TextStyle vs = ui4::style(ui::Font::kE, 12.5f * s,
                                    r.selected ? (r.is_action ? ui4::kInk : ui4::kYellow) : ui4::kWhite,
                                    ui::Align::kRight);
      ui4::fit_text(r.value, vs, rw * 0.36f, 9.0f * s);
      l.text(val_right, ry + 7.0f * s, r.value, vs);
      if (r.selected && !r.is_action) {
        const float tw = ui::List::measure(r.value, vs);
        const float ay = ry + 14.0f * s;
        ui4::opt_arrow(l, val_right - tw - 11.0f * s, ay, arrow_sz, -1, ui4::kWhite);
        ui4::opt_arrow(l, rx1 - 13.0f * s, ay, arrow_sz, +1, ui4::kWhite);
      }

      // Bottom line: real OPT_SLIDER_FRAME + STATBARFILL slider when numeric.
      if (r.norm_bar >= 0.0) {
        ui4::opt_slider(l, rx0 + 14.0f * s, ry + 31.0f * s, rx1 - 14.0f * s, ry + 45.0f * s, float(r.norm_bar), s,
                        r.selected ? ui4::kYellow : ui4::kPink, r.selected);
      } else {
        ui::TextStyle ds = ui4::style(ui::Font::kB, 9.5f * s, r.selected && r.is_action ? ui4::kInk : ui4::kDim);
        const float max_dw = rw - 32.0f * s;
        std::string dtxt = r.detail;
        ui4::fit_text(dtxt, ds, max_dw, 7.5f * s);
        while (dtxt.size() > 6 && ui::List::measure(dtxt, ds) > max_dw) {
          dtxt.resize(dtxt.size() - 4);
          dtxt += "...";
        }
        l.text(rx0 + 14.0f * s, ry + 31.0f * s, dtxt, ds);
      }
      ry += row_h + gap;
    }

    // Compact PANEL01 detail caption tightly sized right below the sliders (no wasted void).
    if (sel_row) {
      const float cy0 = ry + 4.0f * s;
      const float cy1 = cy0 + 58.0f * s;
      ui4::panel01(l, rx0, cy0, rx1, cy1, s, ui::rgba(33, 33, 33, 224), ui4::kPink);
      l.text(rx0 + 14.0f * s, cy0 + 8.0f * s, sel_row->label, ui4::style(ui::Font::kE, 11.5f * s, ui4::kYellow));
      ui::TextStyle ds = ui4::style(ui::Font::kB, 10.5f * s, ui4::kWhite, ui::Align::kLeft, true);
      ui4::fit_text(sel_row->detail, ds, rw - 28.0f * s, 8.5f * s);
      l.text(rx0 + 14.0f * s, cy0 + 28.0f * s, sel_row->detail, ds);
      ry = cy1 + 6.0f * s;
    }

    if (!overlay.status_message().empty()) {
      ui4::pill(l, rx0, ry, rx1, ry + 28.0f * s, ui4::kGreen, ui4::PillKind::kPill26, false);
      l.text(rx0 + 14.0f * s, ry + 7.0f * s, overlay.status_message(), ui4::style(ui::Font::kE, 11.5f * s, ui4::kInk));
    }
    l.unturn();
  }

  ui4::button_bar(l,
                  {{"DPAD_LR", "ADJUST"},
                   {"LB", "TAB"},
                   {"A", "APPLY / RESET"},
                   {"B", "BACK"}},
                  screen_w, screen_h, 56.0f);
}

}  // namespace carview
