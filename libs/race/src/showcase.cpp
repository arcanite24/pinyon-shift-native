#include "race/showcase.h"

#include <algorithm>
#include <cmath>

namespace race {
namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap_angle(double a) {
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

}  // namespace

bool is_airborne_showcase(int career_type, std::string_view horizon_id) {
  if (career_type == 8) return true;
  return horizon_id.rfind("PLANE_RACE_", 0) == 0;
}

ShowcaseSpec showcase_spec_for(std::string_view horizon_id) {
  ShowcaseSpec s;
  s.horizon_id = std::string(horizon_id);
  if (horizon_id == "PLANE_RACE_001") {
    s.kind = ShowcaseAircraftKind::kMustangP51;
    s.display_name = "P-51 MUSTANG";
    s.base_altitude_m = 22.0;
    s.swoop_altitude_m = 13.0;
    s.lateral_sway_m = 14.0;
    s.target_speed_mps = 43.0;
    s.scale = 1.45;
  } else if (horizon_id == "PLANE_RACE_002") {
    s.kind = ShowcaseAircraftKind::kHotAirBalloon;
    s.display_name = "HOT-AIR BALLOON";
    s.base_altitude_m = 36.0;
    s.swoop_altitude_m = 18.0;
    s.lateral_sway_m = 10.0;
    s.target_speed_mps = 46.0;
    s.scale = 1.35;
  } else if (horizon_id == "PLANE_RACE_003") {
    s.kind = ShowcaseAircraftKind::kStuntJet;
    s.display_name = "STUNT JET";
    s.base_altitude_m = 26.0;
    s.swoop_altitude_m = 11.0;
    s.lateral_sway_m = 20.0;
    s.target_speed_mps = 56.0;
    s.scale = 1.2;
  } else if (horizon_id == "PLANE_RACE_004") {
    s.kind = ShowcaseAircraftKind::kBiplane;
    s.display_name = "STUNT BIPLANE";
    s.base_altitude_m = 20.0;
    s.swoop_altitude_m = 9.0;
    s.lateral_sway_m = 14.0;
    s.target_speed_mps = 44.0;
    s.scale = 1.15;
  } else if (horizon_id == "PLANE_RACE_005") {
    s.kind = ShowcaseAircraftKind::kMustangP51;
    s.display_name = "AEROBATIC PLANE";
    s.base_altitude_m = 24.0;
    s.swoop_altitude_m = 10.0;
    s.lateral_sway_m = 18.0;
    s.target_speed_mps = 54.0;
    s.scale = 1.15;
  } else if (horizon_id == "PLANE_RACE_006") {
    s.kind = ShowcaseAircraftKind::kHelicopter;
    s.display_name = "ATTACK HELICOPTER";
    s.base_altitude_m = 19.0;
    s.swoop_altitude_m = 9.5;
    s.lateral_sway_m = 12.0;
    s.target_speed_mps = 50.0;
    s.scale = 1.2;
  } else if (horizon_id == "PLANE_RACE_007") {
    s.kind = ShowcaseAircraftKind::kBiplane;
    s.display_name = "BARNSTORMER BIPLANE";
    s.base_altitude_m = 21.0;
    s.swoop_altitude_m = 9.5;
    s.lateral_sway_m = 15.0;
    s.target_speed_mps = 52.0;
    s.scale = 1.15;
  } else {
    s.kind = ShowcaseAircraftKind::kMustangP51;
    s.display_name = "SHOWCASE AIRCRAFT";
    s.base_altitude_m = 24.0;
    s.swoop_altitude_m = 10.0;
    s.lateral_sway_m = 15.0;
    s.target_speed_mps = 46.0;
    s.scale = 1.15;
  }
  return s;
}

void ShowcaseOpponent::init(const ShowcaseSpec& spec, const drive::RacingLine& line, double start_progress,
                            double finish_progress, double expected_car_speed_mps) {
  spec_ = spec;
  line_ = &line;
  start_p_ = start_progress;
  finish_p_ = std::max(start_progress + 200.0, finish_progress);
  target_speed_ = expected_car_speed_mps > 15.0 ? 0.96 * expected_car_speed_mps : spec_.target_speed_mps;
  active_ = line.size() >= 2;
  pose_ = ShowcasePose{};
  pose_.progress_m = start_p_ + 18.0;  // starts slightly ahead overhead for dramatic launch flyover
  if (active_) {
    double x, y, z, h;
    line_->pose(pose_.progress_m, 0.0, x, y, z, h);
    pose_.position = {x, y + spec_.swoop_altitude_m + 4.0, z};
    pose_.heading = prev_heading_ = h;
  }
}

void ShowcaseOpponent::step(double dt, double race_clock, double player_progress) {
  if (!active_ || !line_ || dt <= 0.0) return;

  // Spin propeller or rotor continuously (even during countdown).
  const double rotor_rate = spec_.kind == ShowcaseAircraftKind::kHotAirBalloon ? 0.4 : 42.0;
  pose_.rotor_angle = std::fmod(pose_.rotor_angle + rotor_rate * dt, 2.0 * kPi);

  const double run_len = std::max(1.0, finish_p_ - start_p_);
  if (race_clock < 0.0) {
    // Countdown dramatic low flyover approaching the start gate.
    const double approach = std::clamp((race_clock + 6.0) / 6.0, 0.0, 1.0);
    const double p = std::max(0.0, start_p_ - 45.0 * (1.0 - approach) + 14.0 * approach);
    pose_.progress_m = p;
    pose_.fraction = 0.0;
    pose_.speed_mps = target_speed_ * 0.75;
  } else if (!pose_.finished) {
    // Calibrated showcase pacing curve: keeps the aircraft excitingly visible
    // overhead through the route while preserving a fair, beatable target time
    // in the final 15% sprint to the finish.
    const double frac = std::clamp((pose_.progress_m - start_p_) / run_len, 0.0, 1.0);
    const double gap = pose_.progress_m - player_progress;  // + when aircraft is ahead
    double pace = target_speed_;
    if (frac < 0.85) {
      // Dramatic low-pass choreography: ease pace if >48 m ahead so the aircraft
      // stays unmistakably visible in frame, push if >70 m behind.
      if (gap > 48.0) pace *= std::clamp(1.0 - (gap - 48.0) / 110.0, 0.45, 1.0);
      else if (gap < -70.0) pace *= std::clamp(1.0 + (-gap - 70.0) / 300.0, 1.0, 1.18);
    } else {
      // Final sprint: honest steady target speed so a clean drive wins.
      pace = target_speed_;
    }
    pose_.speed_mps = pace;
    pose_.progress_m += pace * dt;
    if (pose_.progress_m >= finish_p_) {
      pose_.finished = true;
      pose_.finish_time = race_clock;
    }
    pose_.fraction = std::clamp((pose_.progress_m - start_p_) / run_len, 0.0, 1.0);
  } else {
    // Victory/post-finish flypast beyond the finish line.
    pose_.progress_m = std::min(line_->length() - 5.0, pose_.progress_m + target_speed_ * 0.8 * dt);
    pose_.fraction = 1.0;
  }

  const double p_clamped = std::clamp(pose_.progress_m, 0.0, line_->length());
  double x = 0, y = 0, z = 0, h = 0;
  line_->pose(p_clamped, 0.0, x, y, z, h);
  double xf = 0, yf = 0, zf = 0, hf = 0;
  line_->pose(std::min(line_->length(), p_clamped + 45.0), 0.0, xf, yf, zf, hf);

  // Smooth lateral S-curve weave & swoop altitude profile (swoops low at start, midpoint, and finish!).
  const double frac = std::clamp((p_clamped - start_p_) / run_len, 0.0, 1.0);
  const double weave = spec_.lateral_sway_m * std::sin(frac * 6.0 * kPi);
  const double swoop_env = std::pow(std::sin(frac * 2.0 * kPi), 2.0);  // 0 at start, mid, finish; 1 at 1/4 & 3/4
  double alt = spec_.swoop_altitude_m + (spec_.base_altitude_m - spec_.swoop_altitude_m) * swoop_env;
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    alt += 3.0 * std::sin(race_clock * 0.7);
  }

  const double rx = std::cos(h), rz = -std::sin(h);
  const double target_x = x + rx * weave;
  const double target_z = z + rz * weave;
  const double target_y = std::max(y, yf) + alt;

  const double dx = xf - x, dz = zf - z;
  const double fly_h = std::hypot(dx, dz) > 1e-3 ? std::atan2(dx, dz) : h;
  const double dh = wrap_angle(fly_h - prev_heading_);
  prev_heading_ = fly_h;

  pose_.position = {target_x, target_y, target_z};
  pose_.heading = fly_h;
  if (spec_.kind == ShowcaseAircraftKind::kHotAirBalloon) {
    pose_.pitch = 0.0;
    pose_.roll = 0.06 * std::sin(race_clock * 0.9);
  } else if (spec_.kind == ShowcaseAircraftKind::kHelicopter) {
    pose_.pitch = -0.14;  // forward nose-down hover tilt
    pose_.roll = std::clamp(dh / std::max(0.01, dt) * 0.35, -0.45, 0.45);
  } else {
    pose_.pitch = std::clamp((yf - y) / 45.0, -0.25, 0.25);
    const double bank_from_turn = std::clamp(dh / std::max(0.01, dt) * 0.55, -0.65, 0.65);
    const double bank_from_weave = -0.28 * std::cos(frac * 6.0 * kPi);
    pose_.roll = std::clamp(bank_from_turn + bank_from_weave, -0.75, 0.75);
  }
}

int ShowcaseOpponent::player_place(double player_progress, bool player_finished, double player_finish_time) const {
  if (!active_) return 1;
  if (player_finished && pose_.finished) return player_finish_time <= pose_.finish_time ? 1 : 2;
  if (player_finished) return 1;
  if (pose_.finished) return 2;
  return player_progress >= pose_.progress_m ? 1 : 2;
}

}  // namespace race
