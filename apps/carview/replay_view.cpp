#include "replay_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui4_chrome.h"

namespace carview {
namespace {

constexpr double kSpeedLadder[] = {-4.0, -2.0, -1.0, 0.25, 0.5, 1.0, 2.0, 4.0};
constexpr int kSpeedCount = int(sizeof(kSpeedLadder) / sizeof(kSpeedLadder[0]));

std::array<double, 3> rotate_q(const float q[4], const std::array<double, 3>& v) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  const double tx = 2.0 * (y * v[2] - z * v[1]);
  const double ty = 2.0 * (z * v[0] - x * v[2]);
  const double tz = 2.0 * (x * v[1] - y * v[0]);
  return {
      v[0] + w * tx + (y * tz - z * ty),
      v[1] + w * ty + (z * tx - x * tz),
      v[2] + w * tz + (x * ty - y * tx),
  };
}

std::array<double, 3> normalize3(const std::array<double, 3>& v, const std::array<double, 3>& fallback) {
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len < 1e-6) return fallback;
  return {v[0] / len, v[1] / len, v[2] / len};
}

std::string format_timecode(double s) {
  if (s < 0.0) s = 0.0;
  const int mins = int(s / 60.0);
  const double rem = s - double(mins) * 60.0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%04.1f", mins, rem);
  return buf;
}

std::string format_speed_pill(double speed, bool paused) {
  if (paused) return "PAUSED";
  if (std::fabs(speed - 1.0) < 0.01) return "1.0x PLAY";
  if (std::fabs(speed - 0.5) < 0.01) return "0.5x SLOW";
  if (std::fabs(speed - 0.25) < 0.01) return "0.25x SLOW";
  char buf[24];
  if (speed < 0.0) std::snprintf(buf, sizeof buf, "%.0fx REW", speed);
  else std::snprintf(buf, sizeof buf, "%.0fx FF", speed);
  return buf;
}

}  // namespace

const char* replay_camera_name(ReplayCameraMode m) {
  switch (m) {
    case ReplayCameraMode::kChase: return "CHASE";
    case ReplayCameraMode::kTrackSide: return "TRACK-SIDE";
    case ReplayCameraMode::kOrbit: return "ORBIT";
    case ReplayCameraMode::kBumper: return "BUMPER";
    case ReplayCameraMode::kWheel: return "WHEEL CAM";
    case ReplayCameraMode::kCarRelative: return "CAR-RELATIVE";
    default: return "CHASE";
  }
}

void ReplayViewer::open(drive::ReplayRecording rec, bool start_playing) {
  rec_ = std::move(rec);
  active_ = !rec_.empty();
  t_ = 0.0;
  paused_ = !start_playing;
  speed_ = 1.0;
  focus_slot_ = 0;
  cam_mode_ = ReplayCameraMode::kTrackSide;
  telemetry_ = true;
  ui_hidden_ = false;
  browser_open_ = false;
  trackside_valid_ = false;
  notice_.clear();
  notice_timer_ = 0.0;
  refresh_samples();
}

void ReplayViewer::close() {
  active_ = false;
  browser_open_ = false;
}

void ReplayViewer::toggle_pause() { paused_ = !paused_; }

void ReplayViewer::set_speed(double s) {
  speed_ = std::clamp(s, -8.0, 8.0);
  if (std::fabs(speed_) < 0.05) {
    paused_ = true;
    speed_ = 1.0;
  } else {
    paused_ = false;
  }
}

void ReplayViewer::step_speed(int dir) {
  int best = 5;  // 1.0x
  double best_d = 1e9;
  for (int i = 0; i < kSpeedCount; ++i) {
    const double d = std::fabs(kSpeedLadder[i] - speed_);
    if (d < best_d) best_d = d, best = i;
  }
  const int next = std::clamp(best + dir, 0, kSpeedCount - 1);
  speed_ = kSpeedLadder[next];
  paused_ = false;
}

void ReplayViewer::set_slow_motion() {
  paused_ = false;
  if (std::fabs(speed_ - 0.5) < 0.02) speed_ = 0.25;
  else if (std::fabs(speed_ - 0.25) < 0.02) speed_ = 1.0;
  else speed_ = 0.5;
}

void ReplayViewer::trigger_rewind() {
  paused_ = false;
  if (speed_ > -0.5) speed_ = -1.0;
  else if (speed_ > -1.5) speed_ = -2.0;
  else speed_ = -4.0;
}

void ReplayViewer::trigger_fast_forward() {
  paused_ = false;
  if (speed_ < 1.5) speed_ = 2.0;
  else if (speed_ < 3.0) speed_ = 4.0;
  else speed_ = 1.0;
}

void ReplayViewer::seek(double t_s) {
  const double dur = rec_.duration();
  t_ = std::clamp(t_s, 0.0, dur);
  trackside_valid_ = false;
  refresh_samples();
}

void ReplayViewer::scrub(double delta_s) { seek(t_ + delta_s); }

void ReplayViewer::step_frame(int frames) {
  paused_ = true;
  const double hz = rec_.meta.sample_hz > 1.0 ? rec_.meta.sample_hz : 60.0;
  seek(t_ + double(frames) / hz);
}

void ReplayViewer::cycle_camera(int dir) {
  const int n = int(ReplayCameraMode::kCount);
  cam_mode_ = static_cast<ReplayCameraMode>(((int(cam_mode_) + dir) % n + n) % n);
  trackside_valid_ = false;
}

void ReplayViewer::set_camera_mode(ReplayCameraMode m) {
  cam_mode_ = m;
  trackside_valid_ = false;
}

void ReplayViewer::cycle_focus_car(int dir) {
  if (current_cars_.size() <= 1) return;
  int idx = 0;
  for (size_t i = 0; i < current_cars_.size(); ++i) {
    if (current_cars_[i].slot == focus_slot_) {
      idx = int(i);
      break;
    }
  }
  const int n = int(current_cars_.size());
  focus_slot_ = current_cars_[size_t(((idx + dir) % n + n) % n)].slot;
  trackside_valid_ = false;
}

void ReplayViewer::update(double dt) {
  if (!active_) return;
  dt = std::clamp(dt, 0.0, 0.1);
  if (notice_timer_ > 0.0) notice_timer_ = std::max(0.0, notice_timer_ - dt);
  if (!paused_ && !browser_open_) {
    const double dur = rec_.duration();
    t_ += dt * speed_;
    if (t_ >= dur) {
      t_ = dur;
      paused_ = true;
    } else if (t_ <= 0.0) {
      t_ = 0.0;
      if (speed_ < 0.0) {
        paused_ = true;
        speed_ = 1.0;
      }
    }
  }
  refresh_samples();
}

void ReplayViewer::refresh_samples() { rec_.sample(t_, current_cars_); }

const drive::ReplayCarSample* ReplayViewer::focused_car() const {
  for (const auto& c : current_cars_)
    if (c.slot == focus_slot_) return &c;
  return current_cars_.empty() ? nullptr : &current_cars_.front();
}

void ReplayViewer::reset_trackside_anchor(
    const drive::ReplayCarSample& focus,
    const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below) {
  const auto fwd = normalize3(rotate_q(focus.quat, {0.0, 0.0, 1.0}), {0.0, 0.0, 1.0});
  const auto right = normalize3({fwd[2], 0.0, -fwd[0]}, {1.0, 0.0, 0.0});
  const double lead = std::clamp(double(focus.speed_mps) * 0.75, 11.0, 28.0);
  const double side = 6.2 * double(trackside_side_);
  trackside_side_ = -trackside_side_;
  trackside_eye_ = {
      double(focus.pos[0]) + fwd[0] * lead + right[0] * side,
      double(focus.pos[1]) + 2.4,
      double(focus.pos[2]) + fwd[2] * lead + right[2] * side,
  };
  if (ground_below) {
    double gy = 0.0;
    if (ground_below(trackside_eye_[0], trackside_eye_[1] + 10.0, trackside_eye_[2], gy)) {
      trackside_eye_[1] = std::max(gy + 1.8, double(focus.pos[1]) + 1.6);
    }
  }
  trackside_valid_ = true;
}

drive::CameraView ReplayViewer::compute_camera(
    double dt, float orbit_yaw_in, float orbit_pitch_in, float zoom_in,
    const std::function<bool(double x, double y, double z, double& ground_y)>& ground_below) {
  drive::CameraView v;
  const auto* car = focused_car();
  if (!car) return v;

  const std::array<double, 3> p = {double(car->pos[0]), double(car->pos[1]), double(car->pos[2])};
  const auto fwd = normalize3(rotate_q(car->quat, {0.0, 0.0, 1.0}), {0.0, 0.0, 1.0});
  const auto right = normalize3(rotate_q(car->quat, {1.0, 0.0, 0.0}), {1.0, 0.0, 0.0});
  const auto up = normalize3(rotate_q(car->quat, {0.0, 1.0, 0.0}), {0.0, 1.0, 0.0});
  const std::array<double, 3> target = {p[0], p[1] + 0.75, p[2]};

  switch (cam_mode_) {
    case ReplayCameraMode::kChase: {
      v.eye = {p[0] - fwd[0] * 5.6, p[1] + 2.05, p[2] - fwd[2] * 5.6};
      v.forward = normalize3({target[0] - v.eye[0], target[1] - v.eye[1], target[2] - v.eye[2]}, fwd);
      v.up = {0.0, 1.0, 0.0};
      v.fov_degrees = 52.0;
      break;
    }
    case ReplayCameraMode::kTrackSide: {
      const double dx = p[0] - trackside_eye_[0];
      const double dy = (p[1] + 0.7) - trackside_eye_[1];
      const double dz = p[2] - trackside_eye_[2];
      const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (!trackside_valid_ || dist > 62.0 || dist < 3.0) {
        reset_trackside_anchor(*car, ground_below);
      }
      const double tdx = p[0] - trackside_eye_[0];
      const double tdy = (p[1] + 0.75) - trackside_eye_[1];
      const double tdz = p[2] - trackside_eye_[2];
      const double tdist = std::sqrt(tdx * tdx + tdy * tdy + tdz * tdz);
      v.eye = trackside_eye_;
      v.forward = normalize3({tdx, tdy, tdz}, fwd);
      v.up = {0.0, 1.0, 0.0};
      // Subtle telephoto zoom as the car gets farther from the TV camera.
      v.fov_degrees = std::clamp(48.0 - (tdist - 10.0) * 0.45, 24.0, 48.0);
      break;
    }
    case ReplayCameraMode::kOrbit: {
      dt = std::clamp(dt, 0.0, 0.1);
      orbit_yaw_ += double(orbit_yaw_in) * 1.8 * dt;
      orbit_pitch_ = std::clamp(orbit_pitch_ + double(orbit_pitch_in) * 1.3 * dt, -1.1, 0.15);
      orbit_dist_ = std::clamp(orbit_dist_ - double(zoom_in) * 5.0 * dt, 2.8, 18.0);
      const double heading = std::atan2(fwd[0], fwd[2]) + orbit_yaw_;
      const double cp = std::cos(orbit_pitch_);
      v.eye = {
          target[0] + std::sin(heading) * cp * orbit_dist_,
          target[1] - std::sin(orbit_pitch_) * orbit_dist_ + 0.4,
          target[2] + std::cos(heading) * cp * orbit_dist_,
      };
      if (ground_below) {
        double gy = 0.0;
        if (ground_below(v.eye[0], v.eye[1] + 5.0, v.eye[2], gy)) v.eye[1] = std::max(v.eye[1], gy + 0.35);
      }
      v.forward = normalize3({target[0] - v.eye[0], target[1] - v.eye[1], target[2] - v.eye[2]}, fwd);
      v.up = {0.0, 1.0, 0.0};
      v.fov_degrees = 45.0;
      break;
    }
    case ReplayCameraMode::kBumper: {
      v.eye = {p[0] + fwd[0] * 2.15 + up[0] * 0.52, p[1] + fwd[1] * 2.15 + up[1] * 0.52,
               p[2] + fwd[2] * 2.15 + up[2] * 0.52};
      v.forward = fwd;
      v.up = up;
      v.fov_degrees = 56.0;
      break;
    }
    case ReplayCameraMode::kWheel: {
      // Low fender-mounted camera pointing toward the front-left wheel and apex.
      v.eye = {p[0] - right[0] * 1.55 - fwd[0] * 0.55 + up[0] * 0.55,
               p[1] - right[1] * 1.55 - fwd[1] * 0.55 + up[1] * 0.55,
               p[2] - right[2] * 1.55 - fwd[2] * 0.55 + up[2] * 0.55};
      const std::array<double, 3> wheel_look = {
          p[0] - right[0] * 0.75 + fwd[0] * 1.6 + up[0] * 0.35,
          p[1] - right[1] * 0.75 + fwd[1] * 1.6 + up[1] * 0.35,
          p[2] - right[2] * 0.75 + fwd[2] * 1.6 + up[2] * 0.35,
      };
      v.forward = normalize3({wheel_look[0] - v.eye[0], wheel_look[1] - v.eye[1], wheel_look[2] - v.eye[2]}, fwd);
      v.up = {0.0, 1.0, 0.0};
      v.fov_degrees = 54.0;
      break;
    }
    case ReplayCameraMode::kCarRelative: {
      // Dramatic low 3/4 rear-right chase broadcast angle locked to the car chassis.
      v.eye = {p[0] + right[0] * 2.35 - fwd[0] * 3.6 + up[0] * 1.15,
               p[1] + right[1] * 2.35 - fwd[1] * 3.6 + up[1] * 1.15,
               p[2] + right[2] * 2.35 - fwd[2] * 3.6 + up[2] * 1.15};
      const std::array<double, 3> aim = {p[0] + fwd[0] * 1.2 + up[0] * 0.65,
                                         p[1] + fwd[1] * 1.2 + up[1] * 0.65,
                                         p[2] + fwd[2] * 1.2 + up[2] * 0.65};
      v.forward = normalize3({aim[0] - v.eye[0], aim[1] - v.eye[1], aim[2] - v.eye[2]}, fwd);
      v.up = {0.0, 1.0, 0.0};
      v.fov_degrees = 46.0;
      break;
    }
    default:
      break;
  }
  v.near_clip = 0.22;
  v.interior = false;
  return v;
}

bool ReplayViewer::save_to_dir(const std::filesystem::path& replays_dir, std::filesystem::path& out_path,
                               std::string* error) {
  if (!active_ || rec_.empty()) {
    if (error) *error = "no active replay";
    return false;
  }
  if (!drive::save_replay_auto(replays_dir, rec_, out_path, error)) return false;
  set_notice("REPLAY SAVED: " + out_path.filename().string(), 3.5);
  return true;
}

void ReplayViewer::open_browser(const std::filesystem::path& replays_dir) {
  browser_entries_ = drive::list_replay_files(replays_dir);
  browser_selected_ = 0;
  browser_open_ = true;
}

void ReplayViewer::browser_move(int delta) {
  if (browser_entries_.empty()) return;
  const int n = int(browser_entries_.size());
  browser_selected_ = ((browser_selected_ + delta) % n + n) % n;
}

bool ReplayViewer::browser_load_selected(std::string* error) {
  if (browser_entries_.empty() || browser_selected_ < 0 || size_t(browser_selected_) >= browser_entries_.size())
    return false;
  drive::ReplayRecording loaded;
  if (!drive::load_replay_file(browser_entries_[size_t(browser_selected_)].path, loaded, error)) return false;
  open(std::move(loaded), true);
  set_notice("LOADED: " + browser_entries_[size_t(browser_selected_)].filename, 3.0);
  return true;
}

void ReplayViewer::set_notice(const std::string& msg, double seconds) {
  notice_ = msg;
  notice_timer_ = seconds;
}

void ReplayViewer::build_hud(ui::List& out, float w, float h) const {
  if (!active_ && !browser_open_) return;

  using ui::Align;
  using ui::Font;
  using ui::rgba;
  using ui::TextStyle;

  const float s = h / 720.0f;

  if (browser_open_) {
    ui4::subtle_vignette(out, w, h, 0.35f);
    ui4::title_banner(out, s, 56.0f * s, 28.0f * s, "SAVED REPLAYS", "HORIZON THEATRE", true, 440.0f);
    const float bx0 = 56.0f * s, bx1 = w - 56.0f * s;
    const float by0 = 122.0f * s, by1 = h - 84.0f * s;
    ui4::panel(out, bx0, by0, bx1, by1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui4::offwhite_strip(out, bx0 + 14.0f * s, by0 + 10.0f * s, bx1 - 14.0f * s, by0 + 34.0f * s, s, false);
    out.text(bx0 + 24.0f * s, by0 + 15.0f * s, "RECORDED SESSIONS", ui4::style(Font::kE, 13.0f * s, ui4::kInk));
    if (browser_entries_.empty()) {
      out.text(0.5f * w, 0.5f * h - 10.0f * s, "NO SAVED REPLAYS YET (PRESS S IN REPLAY VIEWER TO SAVE)",
               ui4::style(Font::kB, 14.0f * s, ui4::kDim, Align::kCenter));
    } else {
      const float row_h = 50.0f * s;
      const int max_vis = std::max(1, int((by1 - by0 - 54.0f * s) / row_h));
      const int start = std::max(0, browser_selected_ - max_vis + 1);
      for (int i = start; i < int(browser_entries_.size()) && i < start + max_vis; ++i) {
        const auto& e = browser_entries_[size_t(i)];
        const float ry0 = by0 + 44.0f * s + float(i - start) * row_h;
        const float ry1 = ry0 + row_h - 6.0f * s;
        const bool sel = (i == browser_selected_);
        if (sel) ui4::brush(out, bx0 + 8.0f * s, ry0, bx1 - 8.0f * s, ry1, ui4::kPink);
        else out.rect(bx0 + 14.0f * s, ry0, bx1 - 14.0f * s, ry1,
                      (i % 2 == 0) ? rgba(32, 34, 42, 220) : rgba(24, 26, 32, 220));
        out.text(bx0 + 28.0f * s, ry0 + 7.0f * s, e.event_name + "  [" + format_timecode(e.duration_s) + "]",
                 ui4::style(sel ? Font::kE : Font::kB, 14.0f * s, ui4::kWhite));
        out.text(bx0 + 28.0f * s, ry0 + 25.0f * s, e.filename + (e.player_car.empty() ? "" : ("  •  " + e.player_car)),
                 ui4::style(Font::kB, 10.5f * s, sel ? ui4::kYellow : ui4::kDim));
      }
    }
    ui4::button_bar(out, {{"A", "WATCH REPLAY"}, {"B", "CLOSE"}}, w, h, 56.0f);
    return;
  }

  if (ui_hidden_) return;

  const float mx = 56.0f * s, my = 28.0f * s;
  const auto* focus = focused_car();
  const auto* finfo = rec_.find_car_info(focus ? focus->slot : 0);

  // Top-left REPLAY broadcast header (tilted FH1 brush badge + gritty event card).
  ui4::title_banner(out, s, mx, my, rec_.meta.event_name.empty() ? "REPLAY" : rec_.meta.event_name, "HORIZON REPLAY",
                    true, 440.0f);
  std::string sub = std::string("CAM: ") + replay_camera_name(cam_mode_);
  if (finfo && !finfo->driver_name.empty()) sub += "   •   " + finfo->driver_name;
  else if (finfo && !finfo->car_media.empty()) sub += "   •   " + finfo->car_media;
  ui4::pill(out, mx, my + 78.0f * s, mx + 390.0f * s, my + 104.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu);
  out.text(mx + 14.0f * s, my + 85.0f * s, sub, ui4::style(Font::kB, 11.5f * s, ui4::kWhite));

  // Top-right optional Telemetry HUD panel (sharp gritty FH1 panel).
  if (telemetry_ && focus) {
    const float tx1 = w - mx;
    const float tx0 = tx1 - 286.0f * s;
    const float ty0 = my;
    const float ty1 = ty0 + 168.0f * s;
    ui4::panel(out, tx0, ty0, tx1, ty1, s, ui4::kCharcoalDark, ui4::kYellow, 0, true);
    ui4::offwhite_strip(out, tx0 + 10.0f * s, ty0 + 8.0f * s, tx1 - 10.0f * s, ty0 + 28.0f * s, s, false);
    out.text(tx0 + 18.0f * s, ty0 + 12.0f * s, "TELEMETRY", ui4::style(Font::kE, 11.5f * s, ui4::kInk));
    if (focus->race_place > 0) {
      char pos_buf[24];
      std::snprintf(pos_buf, sizeof pos_buf, "POS %d / %d", int(focus->race_place), std::max(1, rec_.meta.field_size));
      out.text(tx1 - 18.0f * s, ty0 + 12.0f * s, pos_buf, ui4::style(Font::kB, 11.5f * s, ui4::kInk, Align::kRight));
    }

    char spd_buf[32], rpm_buf[32];
    const int kmh = int(std::lround(std::fabs(focus->speed_mps) * 3.6f));
    std::snprintf(spd_buf, sizeof spd_buf, "%3d", kmh);
    std::snprintf(rpm_buf, sizeof rpm_buf, "%4d RPM", std::max(0, int(std::lround(focus->rpm / 10.0f) * 10)));
    out.text(tx0 + 14.0f * s, ty0 + 34.0f * s, spd_buf, ui4::style(Font::kE, 34.0f * s, ui4::kWhite));
    out.text(tx0 + 96.0f * s, ty0 + 54.0f * s, "KM/H", ui4::style(Font::kB, 10.5f * s, ui4::kDim));
    const char gear_ch[2] = {focus->gear < 0 ? 'R' : focus->gear == 0 ? 'N' : char('0' + std::min<int>(focus->gear, 9)),
                             0};
    out.rect(tx1 - 52.0f * s, ty0 + 34.0f * s, tx1 - 14.0f * s, ty0 + 72.0f * s, ui4::kPink);
    out.text(tx1 - 33.0f * s, ty0 + 40.0f * s, gear_ch, ui4::style(Font::kE, 24.0f * s, ui4::kWhite, Align::kCenter));

    // Sharp flat RPM & input bars.
    const float rx0 = tx0 + 14.0f * s, rx1 = tx1 - 14.0f * s, ry0 = ty0 + 82.0f * s, ry1 = ry0 + 7.0f * s;
    const float rpm_f = std::clamp(focus->rpm / std::max(1000.0f, focus->redline_rpm), 0.0f, 1.0f);
    out.rect(rx0, ry0, rx1, ry1, rgba(255, 255, 255, 32));
    out.rect(rx0, ry0, rx0 + (rx1 - rx0) * rpm_f, ry1, rpm_f > 0.88f ? ui4::kRed : ui4::kYellow);
    out.text(tx0 + 96.0f * s, ty0 + 38.0f * s, rpm_buf, ui4::style(Font::kB, 10.5f * s, ui4::kDim));

    auto draw_bar = [&](float y, const char* lbl, float frac, uint32_t col) {
      out.text(rx0, y, lbl, ui4::style(Font::kB, 10.0f * s, ui4::kDim));
      const float bx0 = rx0 + 68.0f * s, bx1 = rx1;
      out.rect(bx0, y + 2.0f * s, bx1, y + 9.0f * s, rgba(255, 255, 255, 28));
      if (frac > 0.01f) out.rect(bx0, y + 2.0f * s, bx0 + (bx1 - bx0) * std::clamp(frac, 0.0f, 1.0f), y + 9.0f * s, col);
    };
    draw_bar(ty0 + 98.0f * s, "THROTTLE", float(focus->throttle_u8) / 255.0f, ui4::kGreen);
    draw_bar(ty0 + 118.0f * s, "BRAKE", float(focus->brake_u8) / 255.0f, ui4::kRed);

    const float sy = ty0 + 138.0f * s;
    out.text(rx0, sy, "STEER", ui4::style(Font::kB, 10.0f * s, ui4::kDim));
    const float sx0 = rx0 + 68.0f * s, sx1 = rx1, smid = 0.5f * (sx0 + sx1);
    out.rect(sx0, sy + 2.0f * s, sx1, sy + 9.0f * s, rgba(255, 255, 255, 28));
    const float st = std::clamp(float(focus->steer_i8) / 127.0f, -1.0f, 1.0f);
    const float stx = smid + st * 0.5f * (sx1 - sx0);
    out.rect(std::min(smid, stx), sy + 2.0f * s, std::max(smid, stx), sy + 9.0f * s, ui4::kAmber);
    out.line(smid, sy, smid, sy + 11.0f * s, 1.5f * s, ui4::kWhite);
  }

  // Optional toast notice.
  if (notice_timer_ > 0.0 && !notice_.empty()) {
    ui4::brush(out, 0.5f * w - 210.0f * s, my + 8.0f * s, 0.5f * w + 210.0f * s, my + 42.0f * s, ui4::kPink);
    out.text(0.5f * w, my + 18.0f * s, notice_, ui4::style(Font::kE, 12.5f * s, ui4::kWhite, Align::kCenter));
  }

  // Bottom 926_REPLAY_HUD gritty timeline scrubber dock (sits cleanly above BUTTON_BAR_SCENE).
  const float dy1 = h - 82.0f * s;
  const float dy0 = dy1 - 88.0f * s;
  ui4::panel(out, mx, dy0, w - mx, dy1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);

  const float top_y = dy0 + 12.0f * s;
  const std::string sp_lbl = format_speed_pill(speed_, paused_);
  ui4::brush(out, mx + 12.0f * s, top_y, mx + 148.0f * s, top_y + 26.0f * s, paused_ ? ui4::kYellow : ui4::kPink);
  out.text(mx + 80.0f * s, top_y + 6.0f * s, sp_lbl,
           ui4::style(Font::kE, 12.5f * s, paused_ ? ui4::kInk : ui4::kWhite, Align::kCenter));

  const char* ladder_lbls[6] = {"<< REW", "0.25x", "0.5x", "1x", "2x", "4x >>"};
  const double ladder_vals[6] = {-2.0, 0.25, 0.5, 1.0, 2.0, 4.0};
  float px = mx + 166.0f * s;
  for (int i = 0; i < 6; ++i) {
    const bool active_sp = !paused_ && ((i == 0 && speed_ < 0.0) || (i > 0 && std::fabs(speed_ - ladder_vals[i]) < 0.05));
    const float pw = 64.0f * s;
    if (active_sp) {
      out.rect(px, top_y + 2.0f * s, px + pw, top_y + 24.0f * s, ui4::kYellow);
    } else {
      ui4::pill(out, px, top_y + 2.0f * s, px + pw, top_y + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kMenu);
    }
    out.text(px + 0.5f * pw, top_y + 7.0f * s, ladder_lbls[i],
             ui4::style(Font::kB, 10.5f * s, active_sp ? ui4::kInk : ui4::kDim, Align::kCenter));
    px += pw + 8.0f * s;
  }

  const std::string tc = format_timecode(t_) + "  /  " + format_timecode(rec_.duration());
  out.text(w - mx - 18.0f * s, top_y + 5.0f * s, tc, ui4::style(Font::kE, 14.5f * s, ui4::kWhite, Align::kRight));

  // Scrubber timeline bar + event markers + playhead.
  const float sx0 = mx + 18.0f * s, sx1 = w - mx - 18.0f * s;
  const float sy0 = dy0 + 50.0f * s, sy1 = sy0 + 10.0f * s;
  out.rect(sx0, sy0, sx1, sy1, rgba(255, 255, 255, 34));
  const float frac = float(std::clamp(progress(), 0.0, 1.0));
  const float head_x = sx0 + (sx1 - sx0) * frac;
  out.rect(sx0, sy0, std::max(sx0 + 6.0f * s, head_x), sy1, ui4::kPink, 0.0f, ui4::kYellow);

  const double dur = rec_.duration();
  if (dur > 0.01) {
    for (const auto& m : rec_.meta.markers) {
      const float mf = float(std::clamp(m.time_s / dur, 0.0, 1.0));
      const float mx_pt = sx0 + (sx1 - sx0) * mf;
      out.line(mx_pt, sy0 - 4.0f * s, mx_pt, sy1 + 4.0f * s, 2.0f * s, ui4::kYellow);
      out.text(mx_pt, sy1 + 6.0f * s, m.label, ui4::style(Font::kB, 9.0f * s, ui4::kYellow, Align::kCenter));
    }
  }
  out.circle(head_x, 0.5f * (sy0 + sy1), 8.5f * s, ui4::kWhite);
  out.circle(head_x, 0.5f * (sy0 + sy1), 4.5f * s, ui4::kPink);

  ui4::button_bar(out,
                  {{"A", "PLAY/PAUSE"},
                   {"DPAD_LR", "SCRUB"},
                   {"Y", "CAMERA"},
                   {"X", "TELEMETRY"},
                   {"LB", "PHOTO"},
                   {"B", "EXIT"}},
                  w, h, 56.0f);
}

}  // namespace carview
