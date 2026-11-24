#include "replay_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "fh1data/stringtables.h"
#include "ui4_chrome.h"

namespace carview {
namespace {

std::function<std::string(const std::string&)>& replay_text_fn() {
  static std::function<std::string(const std::string&)> fn;
  return fn;
}

std::string loc_str(std::string_view table, std::string_view id, const char* fallback) {
  if (const auto& fn = replay_text_fn()) {
    const uint32_t packed = (uint32_t(fh1::string_hash(table)) << 16) | uint32_t(fh1::string_hash(id));
    const std::string ref = "_&" + std::to_string(packed);
    const std::string resolved = fn(ref);
    if (!resolved.empty() && resolved != ref && resolved.rfind("_&", 0) != 0) return resolved;
  }
  return fallback;
}

std::string clean_unit_str(std::string s, const char* fallback) {
  if (auto p = s.find("{0}"); p != std::string::npos) s.erase(p, 3);
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s.empty() ? std::string(fallback) : s;
}

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
  if (paused) return loc_str("InGame", "IDS_Paused", "PAUSED");
  if (std::fabs(speed - 1.0) < 0.01) return "1.0x PLAY";
  if (std::fabs(speed - 0.5) < 0.01) return "0.5x SLOW";
  if (std::fabs(speed - 0.25) < 0.01) return "0.25x SLOW";
  char buf[24];
  if (speed < 0.0) std::snprintf(buf, sizeof buf, "%.0fx REW", speed);
  else std::snprintf(buf, sizeof buf, "%.0fx FF", speed);
  return buf;
}

}  // namespace

void ReplayViewer::set_text_resolver(std::function<std::string(const std::string&)> fn) {
  replay_text_fn() = std::move(fn);
}

std::string format_replay_speed(float speed_mps, bool metric_units) {
  const float spd = std::fabs(speed_mps);
  char buf[48];
  if (metric_units) {
    const int val = int(std::lround(spd * 3.6f));
    const std::string unit = clean_unit_str(loc_str("UnitStrings", "IDS_Units_KMH", "KM/H"), "KM/H");
    std::snprintf(buf, sizeof buf, "%3d %s", val, unit.c_str());
  } else {
    const int val = int(std::lround(spd * 2.2369363f));
    const std::string unit = clean_unit_str(loc_str("UnitStrings", "IDS_Units_MPH", "MPH"), "MPH");
    std::snprintf(buf, sizeof buf, "%3d %s", val, unit.c_str());
  }
  return buf;
}

std::string format_replay_boost(float boost_psi, bool has_fi, bool metric_units) {
  if (!has_fi) return "N/A";
  const float psi = std::max(0.0f, boost_psi);
  char buf[32];
  if (metric_units) {
    std::snprintf(buf, sizeof buf, "%.2f BAR", psi * 0.0689476f);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f PSI", psi);
  }
  return buf;
}

std::string format_replay_power(float power_kw, bool metric_units) {
  const float kw = std::max(0.0f, power_kw);
  char buf[32];
  if (metric_units) {
    std::snprintf(buf, sizeof buf, "%.0f kW", kw);
  } else {
    std::snprintf(buf, sizeof buf, "%.0f HP", kw * 1.341022f);
  }
  return buf;
}

std::string format_replay_torque(float torque_nm, bool metric_units) {
  const float nm = std::max(0.0f, torque_nm);
  char buf[32];
  if (metric_units) {
    std::snprintf(buf, sizeof buf, "%.0f Nm", nm);
  } else {
    std::snprintf(buf, sizeof buf, "%.0f LB-FT", nm * 0.737562f);
  }
  return buf;
}

std::string format_replay_temp(float temp_c, bool metric_units) {
  char buf[32];
  if (metric_units) {
    std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0" "C", temp_c);
  } else {
    std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0" "F", temp_c * 9.0f / 5.0f + 32.0f);
  }
  return buf;
}

const char* replay_camera_name(ReplayCameraMode m) {
  switch (m) {
    case ReplayCameraMode::kChase: return "CHASE";
    case ReplayCameraMode::kTrackSide: return "TRACK-SIDE";
    case ReplayCameraMode::kOrbit: return "ORBIT";
    case ReplayCameraMode::kBumper: return "BUMPER";
    case ReplayCameraMode::kWheel: return "WHEEL CAM";
    case ReplayCameraMode::kCarRelative: return "CAR-RELATIVE";
    case ReplayCameraMode::kCockpit: return "COCKPIT";
    case ReplayCameraMode::kHood: return "HOOD";
    default: return "CHASE";
  }
}

const char* replay_telemetry_page_name(ReplayTelemetryPage p) {
  switch (p) {
    case ReplayTelemetryPage::kGeneral: return "GENERAL";
    case ReplayTelemetryPage::kFriction: return "FRICTION & TYRES";
    case ReplayTelemetryPage::kSuspension: return "SUSPENSION";
    case ReplayTelemetryPage::kDamage: return "DAMAGE";
    case ReplayTelemetryPage::kEngine: return "ENGINE & DRIVE";
    default: return "GENERAL";
  }
}

void ReplayViewer::cycle_telemetry_page(int dir) {
  const int n = int(ReplayTelemetryPage::kCount);
  telemetry_page_ = static_cast<ReplayTelemetryPage>(((int(telemetry_page_) + dir) % n + n) % n);
  telemetry_ = true;
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
      if (repeat_loop_ && dur > 0.05) {
        t_ = 0.0;
        trackside_valid_ = false;
      } else {
        t_ = dur;
        paused_ = true;
      }
    } else if (t_ <= 0.0) {
      t_ = 0.0;
      if (speed_ < 0.0) {
        if (repeat_loop_ && dur > 0.05) {
          t_ = dur;
          trackside_valid_ = false;
        } else {
          paused_ = true;
          speed_ = 1.0;
        }
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
    case ReplayCameraMode::kCockpit: {
      // Driver eye position inside cockpit looking out the windscreen.
      v.eye = {p[0] - right[0] * 0.36 - fwd[0] * 0.22 + up[0] * 1.12,
               p[1] - right[1] * 0.36 - fwd[1] * 0.22 + up[1] * 1.12,
               p[2] - right[2] * 0.36 - fwd[2] * 0.22 + up[2] * 1.12};
      v.forward = fwd;
      v.up = up;
      v.fov_degrees = 52.0;
      v.near_clip = 0.12;
      v.interior = true;
      return v;
    }
    case ReplayCameraMode::kHood: {
      // Hood-mounted forward broadcast view.
      v.eye = {p[0] + fwd[0] * 0.55 + up[0] * 1.14, p[1] + fwd[1] * 0.55 + up[1] * 1.14,
               p[2] + fwd[2] * 0.55 + up[2] * 1.14};
      v.forward = fwd;
      v.up = up;
      v.fov_degrees = 54.0;
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

void ReplayViewer::browser_select(int idx) {
  if (browser_entries_.empty()) return;
  browser_selected_ = std::clamp(idx, 0, int(browser_entries_.size()) - 1);
}

void ReplayViewer::seek_fraction(double frac) {
  const double dur = rec_.duration();
  if (dur <= 0.0) return;
  t_ = std::clamp(frac, 0.0, 1.0) * dur;
  refresh_samples();
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
    ui4::title_banner(out, s, 56.0f * s, 28.0f * s, loc_str("Replay", "IDS_SavedReplays", "SAVED REPLAYS"),
                      "HORIZON THEATRE", true, 440.0f);
    const float bx0 = 56.0f * s, bx1 = w - 56.0f * s;
    const float by0 = 122.0f * s, by1 = h - 84.0f * s;
    ui4::panel(out, bx0, by0, bx1, by1, s, ui4::kCharcoalDark, ui4::kPink, 0, true);
    ui4::offwhite_strip(out, bx0 + 14.0f * s, by0 + 10.0f * s, bx1 - 14.0f * s, by0 + 34.0f * s, s, false);
    out.text(bx0 + 24.0f * s, by0 + 15.0f * s, loc_str("Replay", "IDS_RecordedSessions", "RECORDED SESSIONS"),
             ui4::style(Font::kE, 13.0f * s, ui4::kInk));
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
        out.add_hit(bx0 + 8.0f * s, ry0, bx1 - 8.0f * s, ry1, ui::kHitRow, i);
        if (sel) ui4::brush(out, bx0 + 8.0f * s, ry0, bx1 - 8.0f * s, ry1, ui4::kPink);
        else out.rect(bx0 + 14.0f * s, ry0, bx1 - 14.0f * s, ry1,
                      (i % 2 == 0) ? rgba(32, 34, 42, 220) : rgba(24, 26, 32, 220));
        out.text(bx0 + 28.0f * s, ry0 + 7.0f * s, e.event_name + "  [" + format_timecode(e.duration_s) + "]",
                 ui4::style(sel ? Font::kE : Font::kB, 14.0f * s, ui4::kWhite));
        out.text(bx0 + 28.0f * s, ry0 + 25.0f * s, e.filename + (e.player_car.empty() ? "" : ("  •  " + e.player_car)),
                 ui4::style(Font::kB, 10.5f * s, sel ? ui4::kYellow : ui4::kDim));
      }
    }
    ui4::button_bar(out,
                    {{"A", loc_str("HelpButtons", "IDS_Select", "WATCH REPLAY")},
                     {"B", loc_str("HelpButtons", "IDS_Back", "CLOSE")}},
                    w, h, 56.0f);
    return;
  }

  if (ui_hidden_) return;

  const float ox = 0.5f * w - 640.0f * s, oy = 0.5f * h - 360.0f * s;
  auto fx = [&](float cx) { return ox + cx * s; };
  auto fy = [&](float cy) { return oy + cy * s; };

  const auto* focus = focused_car();
  const auto* finfo = rec_.find_car_info(focus ? focus->slot : 0);

  // 1. 926_REPLAY_HUD CURRENT_PLAYER@(640,32): slim top-center driver/camera HUDPILL01
  //    (replaces the raw top-left dark box!).
  {
    std::string drv = finfo && !finfo->driver_name.empty()
                          ? finfo->driver_name
                          : (finfo && !finfo->car_media.empty() ? finfo->car_media : "PLAYER");
    std::string hdr = drv + "   •   " + replay_camera_name(cam_mode_);
    if (!rec_.meta.event_name.empty()) hdr = rec_.meta.event_name + "   •   " + hdr;

    ui::TextStyle pts = ui4::style(Font::kE, 14.0f * s, ui4::kWhite, Align::kCenter);
    const float pw = std::clamp(ui::List::measure(hdr, pts) + 46.0f * s, 260.0f * s, 720.0f * s);
    const float cx = 0.5f * w;
    const float py0 = fy(22.0f), py1 = py0 + 28.0f * s;
    ui4::pill(out, cx - 0.5f * pw, py0, cx + 0.5f * pw, py1, ui4::kPillGrey, ui4::PillKind::kHud01);
    out.rect(cx - 0.5f * pw, py0, cx - 0.5f * pw + 4.0f * s, py1, ui4::kPink);
    out.text(cx, py0 + 7.0f * s, hdr, pts);
  }

  // 2. Slim 926_REPLAY_HUD / 968_TELEMETRY right-hand HUDPILL01 strips + I_HUD_DAMAGE_* icon
  //    using real recorded/interpolated telemetry and respecting metric_units_.
  if (telemetry_ && focus) {
    const float rx1 = fx(1216.0f);
    const float rx0 = fx(932.0f);
    float ry = fy(64.0f);

    // Speed + Gear slim HUDPILL01
    ui4::pill(out, rx0, ry, rx1, ry + 34.0f * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    const std::string spd_str = format_replay_speed(focus->speed_mps, metric_units_);
    out.text(rx0 + 12.0f * s, ry + 7.0f * s, spd_str, ui4::style(Font::kE, 19.0f * s, ui4::kWhite));
    const char gear_ch[2] = {focus->gear < 0 ? 'R' : focus->gear == 0 ? 'N' : char('0' + std::min<int>(focus->gear, 9)),
                             0};
    out.rect(rx1 - 36.0f * s, ry + 2.0f * s, rx1 - 4.0f * s, ry + 32.0f * s, ui4::kPink);
    out.text(rx1 - 20.0f * s, ry + 7.0f * s, gear_ch, ui4::style(Font::kE, 19.0f * s, ui4::kWhite, Align::kCenter));
    ry += 38.0f * s;

    // Real telemetry bar row: label | bar | right-aligned numeric/unit readout.
    auto telem_bar = [&](const char* lbl, float frac, uint32_t col, bool centered, const std::string& val_txt) {
      ui4::pill(out, rx0, ry, rx1, ry + 22.0f * s, ui4::kPillGrey, ui4::PillKind::kHud02);
      out.text(rx0 + 10.0f * s, ry + 5.0f * s, lbl, ui4::style(Font::kB, 10.5f * s, ui4::kDim));
      const float bx0 = rx0 + 74.0f * s;
      const float bx1 = val_txt.empty() ? (rx1 - 10.0f * s) : (rx1 - 74.0f * s);
      const float by0 = ry + 7.0f * s, by1 = ry + 15.0f * s;
      out.rect(bx0, by0, bx1, by1, rgba(255, 255, 255, 28));
      if (centered) {
        const float mid = 0.5f * (bx0 + bx1);
        const float stx = mid + std::clamp(frac, -1.0f, 1.0f) * 0.5f * (bx1 - bx0);
        out.rect(std::min(mid, stx), by0, std::max(mid, stx), by1, col);
        out.line(mid, by0 - 1.0f * s, mid, by1 + 1.0f * s, 1.5f * s, ui4::kWhite);
      } else if (frac > 0.005f) {
        out.rect(bx0, by0, bx0 + (bx1 - bx0) * std::clamp(frac, 0.0f, 1.0f), by1, col);
      }
      if (!val_txt.empty()) {
        out.text(rx1 - 8.0f * s, ry + 5.0f * s, val_txt,
                 ui4::style(Font::kB, 10.5f * s, ui4::kWhite, Align::kRight, true));
      }
      ry += 25.0f * s;
    };
    // Page header pill (968_TELEMETRY cycleable pages: General / Friction / Suspension / Damage / Engine).
    ui4::pill(out, rx0, ry, rx1, ry + 22.0f * s, ui4::kPink, ui4::PillKind::kHud02);
    const std::string tel_hdr = loc_str("Replay", "IDS_Telemetry", "TELEMETRY");
    char pbuf[96];
    std::snprintf(pbuf, sizeof pbuf, "%s %d/%d  •  %s", tel_hdr.c_str(), int(telemetry_page_) + 1,
                  int(ReplayTelemetryPage::kCount), replay_telemetry_page_name(telemetry_page_));
    out.text(rx0 + 10.0f * s, ry + 5.0f * s, pbuf, ui4::style(Font::kE, 10.5f * s, ui4::kWhite));
    ry += 25.0f * s;

    const float rpm_f = std::clamp(focus->rpm / std::max(1000.0f, focus->redline_rpm), 0.0f, 1.0f);
    const float thr_f = float(focus->throttle_u8) / 255.0f;
    const float brk_f = float(focus->brake_u8) / 255.0f;
    const float str_f = float(focus->steer_i8) / 127.0f;
    auto fmt_pct = [](float f) {
      char b[16];
      std::snprintf(b, sizeof b, "%d%%", int(std::lround(std::clamp(f, 0.0f, 1.0f) * 100.0f)));
      return std::string(b);
    };
    auto health_col = [](float h) { return h >= 0.75f ? ui4::kGreen : (h >= 0.40f ? ui4::kAmber : ui4::kRed); };

    if (telemetry_page_ == ReplayTelemetryPage::kGeneral) {
      char rbuf[24], sbuf[24];
      std::snprintf(rbuf, sizeof rbuf, "%d", int(std::lround(std::max(0.0f, focus->rpm))));
      std::snprintf(sbuf, sizeof sbuf, "%+d%%", int(std::lround(str_f * 100.0f)));
      telem_bar("RPM", rpm_f, rpm_f > 0.88f ? ui4::kPink : ui4::kYellow, false, rbuf);
      telem_bar("THROTTLE", thr_f, ui4::kGreen, false, fmt_pct(thr_f));
      telem_bar("BRAKE", brk_f, ui4::kRed, false, fmt_pct(brk_f));
      telem_bar("STEER", str_f, ui4::kAmber, true, sbuf);
    } else if (telemetry_page_ == ReplayTelemetryPage::kFriction) {
      static constexpr const char* kWheelLbl[4] = {"FL TYRE", "FR TYRE", "RL TYRE", "RR TYRE"};
      for (int i = 0; i < 4; ++i) {
        const float slip = std::max(0.0f, focus->wheel_slip[i]);
        const float norm = std::clamp(slip / 1.25f, 0.0f, 1.0f);
        const uint32_t col = slip > 1.05f ? ui4::kPink : (slip > 0.65f ? ui4::kYellow : ui4::kGreen);
        telem_bar(kWheelLbl[i], norm, col, false, format_replay_temp(focus->wheel_temp_c[i], metric_units_));
      }
    } else if (telemetry_page_ == ReplayTelemetryPage::kSuspension) {
      static constexpr const char* kCompLbl[4] = {"FL COMP", "FR COMP", "RL COMP", "RR COMP"};
      for (int i = 0; i < 4; ++i) {
        const float comp = std::clamp(focus->wheel_compression[i], 0.0f, 1.0f);
        const uint32_t col = comp > 0.85f ? ui4::kPink : (i < 2 ? ui4::kAmber : ui4::kYellow);
        telem_bar(kCompLbl[i], comp, col, false, fmt_pct(comp));
      }
    } else if (telemetry_page_ == ReplayTelemetryPage::kDamage) {
      static constexpr const char* kDmgLbl[4] = {"ENGINE", "TRANS", "SUSP", "AERO"};
      for (int i = 0; i < 4; ++i) {
        const float health = std::clamp(1.0f - focus->damage[i], 0.0f, 1.0f);
        telem_bar(kDmgLbl[i], health, health_col(health), false, fmt_pct(health));
      }
    } else {
      const bool has_fi = focus->boost_max_psi > 0.5f;
      const float pwr_f = std::clamp(std::max(0.0f, focus->engine_power_kw) / 600.0f, 0.0f, 1.0f);
      const float trq_f = std::clamp(std::max(0.0f, focus->engine_torque_nm) / 800.0f, 0.0f, 1.0f);
      const float bst_f = has_fi ? std::clamp(focus->boost_psi / focus->boost_max_psi, 0.0f, 1.0f) : 0.0f;
      char rbuf[24];
      std::snprintf(rbuf, sizeof rbuf, "%d", int(std::lround(std::max(0.0f, focus->rpm))));
      telem_bar("POWER", pwr_f, ui4::kGreen, false, format_replay_power(focus->engine_power_kw, metric_units_));
      telem_bar("TORQUE", trq_f, ui4::kYellow, false, format_replay_torque(focus->engine_torque_nm, metric_units_));
      telem_bar("BOOST", bst_f, ui4::kAmber, false, format_replay_boost(focus->boost_psi, has_fi, metric_units_));
      telem_bar("RPM", rpm_f, rpm_f > 0.88f ? ui4::kPink : ui4::kWhite, false, rbuf);
    }

    // Slim 926_REPLAY_HUD / 968_TELEMETRY chassis status strip (I_HUD_DAMAGE_ENGINE glyph) using real damage health.
    const float avg_dmg =
        0.25f * (focus->damage[0] + focus->damage[1] + focus->damage[2] + focus->damage[3]);
    const float chassis_h = std::clamp(1.0f - avg_dmg, 0.0f, 1.0f);
    const uint32_t ch_col = health_col(chassis_h);
    char ch_buf[48];
    std::snprintf(ch_buf, sizeof ch_buf, "CHASSIS & ENGINE  %d%%", int(std::lround(chassis_h * 100.0f)));
    std::array<float, 4> dmg_uv{};
    if (ui4::texture("T:I_MAP/I_HUD/I_HUD_DAMAGE_ENGINE", dmg_uv)) {
      ui4::pill(out, rx0, ry, rx1, ry + 24.0f * s, ui4::kPillGrey, ui4::PillKind::kHud02);
      out.image(rx0 + 8.0f * s, ry + 3.0f * s, rx0 + 26.0f * s, ry + 21.0f * s, dmg_uv, ch_col);
      out.text(rx0 + 34.0f * s, ry + 5.0f * s, ch_buf, ui4::style(Font::kB, 11.0f * s, ch_col));
    }
  }

  // Optional toast notice on tilted brush.
  if (notice_timer_ > 0.0 && !notice_.empty()) {
    ui4::brush(out, 0.5f * w - 200.0f * s, fy(58.0f), 0.5f * w + 200.0f * s, fy(88.0f), ui4::kPink);
    out.text(0.5f * w, fy(66.0f), notice_, ui4::style(Font::kE, 13.0f * s, ui4::kWhite, Align::kCenter));
  }

  // 3. 926_REPLAY_HUD Group_FOOTER@(640,721): authentic DVR transport buttons
  //    (T:UI4/HUD/DVR_BUTTON_*) + PILL_BIG / PILL36PX timeline bar — ZERO dark dev box!
  {
    const float foot_y = fy(574.0f);
    const float bar_x0 = fx(70.0f), bar_x1 = fx(1210.0f);

    // Transport icon row using real T:UI4/HUD/DVR_BUTTON_* textures from Textures.zip!
    struct DvrBtn {
      const char* tex;
      const char* fallback;
      bool active;
    };
    const DvrBtn btns[7] = {
        {"T:UI4/HUD/DVR_BUTTON_RR", "<<", !paused_ && speed_ < 0.0},
        {"T:UI4/HUD/DVR_BUTTON_PLAY", ">", !paused_ && speed_ > 0.0 && speed_ <= 1.05},
        {"T:UI4/HUD/DVR_BUTTON_PAUSE", "||", paused_},
        {"T:UI4/HUD/DVR_BUTTON_FF", ">>", !paused_ && speed_ > 1.05},
        {"T:UI4/HUD/DVR_BUTTON_CAMERA", "CAM", false},
        {"T:UI4/HUD/DVR_BUTTON_TELEMETRY", "TEL", telemetry_},
        {"T:UI4/HUD/DVR_BUTTON_REPEAT", "RPT", repeat_loop_},
    };

    // Backing PILL36PX strip behind DVR transport + timecode.
    std::array<float, 4> pill36{};
    if (ui4::texture("KITPARTS/PILL36PX", pill36)) {
      out.image(bar_x0, foot_y, bar_x1, foot_y + 38.0f * s, pill36, ui4::kPillGrey);
    } else {
      ui4::pill(out, bar_x0, foot_y, bar_x1, foot_y + 38.0f * s, ui4::kPillGrey, ui4::PillKind::kHud01);
    }

    float bx = bar_x0 + 14.0f * s;
    for (int bi = 0; bi < 7; ++bi) {
      const auto& b = btns[bi];
      std::array<float, 4> buv{};
      const uint32_t col = b.active ? ui4::kPink : ui4::kWhite;
      out.add_hit(bx - 3.0f * s, foot_y + 3.0f * s, bx + 31.0f * s, foot_y + 35.0f * s, ui::kHitRow, bi);
      if (b.active) {
        out.rect(bx - 3.0f * s, foot_y + 3.0f * s, bx + 31.0f * s, foot_y + 35.0f * s, ui::fade(ui4::kPink, 0.35));
      }
      if (ui4::texture(b.tex, buv)) {
        out.image(bx, foot_y + 5.0f * s, bx + 28.0f * s, foot_y + 33.0f * s, buv, col);
      } else {
        out.rect(bx, foot_y + 7.0f * s, bx + 28.0f * s, foot_y + 31.0f * s,
                 b.active ? ui4::kPink : ui::rgba(46, 48, 56, 220));
        out.text(bx + 14.0f * s, foot_y + 13.0f * s, b.fallback,
                 ui4::style(Font::kB, 10.0f * s, ui4::kWhite, Align::kCenter));
      }
      bx += 38.0f * s;
    }

    // Speed badge + timecode right-aligned inside Group_FOOTER strip.
    const std::string sp_lbl = format_speed_pill(speed_, paused_);
    ui4::brush(out, bx + 8.0f * s, foot_y + 5.0f * s, bx + 120.0f * s, foot_y + 33.0f * s,
               paused_ ? ui4::kYellow : ui4::kPink);
    out.text(bx + 64.0f * s, foot_y + 12.0f * s, sp_lbl,
             ui4::style(Font::kE, 12.5f * s, paused_ ? ui4::kInk : ui4::kWhite, Align::kCenter));

    const std::string tc = format_timecode(t_) + "  /  " + format_timecode(rec_.duration());
    out.text(bar_x1 - 16.0f * s, foot_y + 11.0f * s, tc, ui4::style(Font::kE, 15.0f * s, ui4::kWhite, Align::kRight));

    // Clean sharp timeline scrubber strip (PILL_BIG / BARBASE8PX, ZERO rounded box!).
    const float sy0 = foot_y + 44.0f * s, sy1 = sy0 + 10.0f * s;
    out.add_hit(bar_x0, sy0 - 6.0f * s, bar_x1, sy1 + 6.0f * s, ui4::kHitTimeline, 0);
    out.rect(bar_x0, sy0, bar_x1, sy1, ui::rgba(14, 14, 14, 215));
    const float frac = float(std::clamp(progress(), 0.0, 1.0));
    const float head_x = bar_x0 + (bar_x1 - bar_x0) * frac;
    out.rect(bar_x0, sy0, std::max(bar_x0 + 4.0f * s, head_x), sy1, ui4::kPink);

    const double dur = rec_.duration();
    if (dur > 0.01) {
      for (const auto& m : rec_.meta.markers) {
        const float mf = float(std::clamp(m.time_s / dur, 0.0, 1.0));
        const float mx_pt = bar_x0 + (bar_x1 - bar_x0) * mf;
        out.line(mx_pt, sy0 - 3.0f * s, mx_pt, sy1 + 3.0f * s, 2.0f * s, ui4::kYellow);
      }
    }
    out.rect(head_x - 3.0f * s, sy0 - 3.0f * s, head_x + 3.0f * s, sy1 + 3.0f * s, ui4::kWhite);
  }

  // Authentic BUTTON_BAR_SCENE bottom prompts (localized via string tables with English fallbacks).
  ui4::button_bar(out,
                  {{"A", loc_str("Replay", "IDS_PlayPause", "PLAY/PAUSE")},
                   {"DPAD_LR", loc_str("Replay", "IDS_Scrub", "SCRUB")},
                   {"Y", loc_str("Replay", "IDS_Camera", "CAMERA")},
                   {"X", loc_str("Replay", "IDS_Telemetry", "TELEMETRY")},
                   {"LB", loc_str("PauseMenu", "IDS_Photo", "PHOTO")},
                   {"B", loc_str("HelpButtons", "IDS_Back", "EXIT")}},
                  w, h, 70.0f);
}

}  // namespace carview
