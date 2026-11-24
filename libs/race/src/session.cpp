#include "race/session.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace race {

int player_slot(int p, int cars) {
  if (p >= 1 && p <= cars) return p - 1;
  return std::max(0, cars - 1);
}

int faked_pi_offset(int64_t ordinal, bool flagged) {
  if (flagged) return 5;
  const uint32_t x = uint32_t(214013u * uint32_t(ordinal) + 2531011u);
  const int k = int((x >> 16) & 0x7FFFu) % 5;
  return (k & 1) ? k : -k;
}

AiUpgradeScales ai_upgrade_scales(double t) {
  t = std::clamp(t, -1.0, 1.0);
  AiUpgradeScales s;
  s.t = t;
  if (t >= 0.0) {
    // SPEC-RACE-AI up: mass 1 - 0.1*t, torque 1 + 0.4*t, friction 1 + 0.20*t.
    s.mass_scale = 1.0 - 0.1 * t;
    s.torque_scale = 1.0 + 0.4 * t;
    s.friction_scale = 1.0 + 0.20 * t;
  } else {
    // SPEC-RACE-AI down (u = |t|): mass 1 + 0.111*u, torque 1 - 0.286*u, friction 1 - 0.167*u.
    const double u = -t;
    s.mass_scale = 1.0 + (1.0 / 0.9 - 1.0) * u;
    s.torque_scale = 1.0 - (1.0 - 1.0 / 1.4) * u;
    s.friction_scale = 1.0 - (1.0 - 1.0 / 1.2) * u;
  }
  return s;
}

void apply_ai_upgrade(drive::CarParams& params, double t, int tuning_level) {
  const AiUpgradeScales s = ai_upgrade_scales(t);
  params.mass *= s.mass_scale;
  params.torque_scale *= s.torque_scale;
  for (int a = 0; a < 2; ++a) {
    params.long_scale[size_t(a)] *= s.friction_scale;
    params.lat_scale[size_t(a)] *= s.friction_scale;
  }
  const int lvl = std::clamp(tuning_level, 0, 3);
  if (lvl > 0) {
    // EventParticipants.TuningLevel (1=street, 2=sport, 3=race): progressive suspension
    // damping/antiroll stiffening and tighter shift/final-drive gearing polish.
    const double spring_k = 1.0 + 0.05 * double(lvl);
    const double damp_k = 1.0 + 0.08 * double(lvl);
    const double arb_k = 1.0 + 0.10 * double(lvl);
    const double shift_k = 1.0 - 0.12 * double(lvl);
    for (int a = 0; a < 2; ++a) {
      params.spring_rate[size_t(a)] *= spring_k;
      params.bump_rate[size_t(a)] *= damp_k;
      params.rebound_rate[size_t(a)] *= damp_k;
      params.antiroll_rate[size_t(a)] *= arb_k;
    }
    params.gear_shift_time *= shift_k;
    params.clutch_in_time *= shift_k;
    params.clutch_out_time *= shift_k;
    params.rev_match_time *= shift_k;
    params.final_drive *= (1.0 + 0.015 * double(lvl));
  }
}

AiUpgradeScales solve_ai_upgrade(const drive::CarParams& stock, double stock_pi, double target_pi,
                                 const std::function<double(const drive::CarParams&)>& rate_pi, int iterations) {
  AiUpgradeScales out = ai_upgrade_scales(0.0);
  out.rated_pi = stock_pi;
  if (!rate_pi || std::fabs(target_pi - stock_pi) < 0.25) return out;
  const bool up = target_pi > stock_pi;
  const double far_t = up ? 1.0 : -1.0;
  drive::CarParams far_p = stock;
  apply_ai_upgrade(far_p, far_t, 0);
  const double far_pi = rate_pi(far_p);
  if ((up && far_pi <= target_pi) || (!up && far_pi >= target_pi)) {
    out = ai_upgrade_scales(far_t);
    out.rated_pi = far_pi;
    return out;
  }
  // Bisect `iterations` (10) times between the low-PI end and high-PI end.
  double lo = up ? 0.0 : -1.0;
  double hi = up ? 1.0 : 0.0;
  double best_t = 0.0;
  double best_pi = stock_pi;
  double best_err = std::fabs(stock_pi - target_pi);
  const int iters = std::max(1, iterations);
  for (int it = 0; it < iters; ++it) {
    const double mid = 0.5 * (lo + hi);
    drive::CarParams pm = stock;
    apply_ai_upgrade(pm, mid, 0);
    const double mid_pi = rate_pi(pm);
    const double err = std::fabs(mid_pi - target_pi);
    if (err < best_err) {
      best_err = err;
      best_t = mid;
      best_pi = mid_pi;
    }
    if (mid_pi > target_pi) hi = mid;
    else lo = mid;
  }
  const double final_t = 0.5 * (lo + hi);
  drive::CarParams pf = stock;
  apply_ai_upgrade(pf, final_t, 0);
  const double final_pi = rate_pi(pf);
  if (std::fabs(final_pi - target_pi) <= best_err) {
    best_t = final_t;
    best_pi = final_pi;
  }
  out = ai_upgrade_scales(best_t);
  out.rated_pi = best_pi;
  return out;
}

std::vector<int> opponent_slots(int player, int opponents) {
  std::vector<int> out;
  for (int s = 0; int(out.size()) < opponents; ++s)
    if (s != player) out.push_back(s);
  return out;
}

Session::Session(size_t cars, bool circuit, int laps, const Gate& start, const std::optional<Gate>& finish,
                 const Options& o)
    : cars_(cars), progress_(cars, 0), circuit_(circuit), laps_(std::max(1, laps)), start_(start), finish_(finish),
      o_(o), clock_(-o.countdown) {}

int Session::digit() const {
  if (clock_ >= 0 || clock_ < -3) return 0;
  return int(std::ceil(-clock_));
}

int Session::lap(size_t i) const {
  const Car& c = cars_[i];
  if (!circuit_) return 1;
  return std::clamp(c.crossings, 0, laps_);
}

bool Session::all_finished() const {
  for (const auto& c : cars_)
    if (c.finish < 0) return false;
  return true;
}

void Session::reset_done(size_t i) {
  Car& c = cars_[i];
  c.wrong = c.slow = c.off5 = c.off15 = c.rolled = 0;
  c.reset_fade_out = 0.0;
  c.reset_fade_in_timer = std::max(0.0, o_.reset_fade_in);
  c.no_collide_until = clock_ + 3.0;  // PostResetNonCollideTime
}

double Session::reset_fade_alpha(size_t i) const {
  if (i >= cars_.size()) return 1.0;
  const Car& c = cars_[i];
  if (c.reset_fade_in_timer > 0.0 && o_.reset_fade_in > 1e-6) {
    return std::clamp(1.0 - c.reset_fade_in_timer / o_.reset_fade_in, 0.0, 1.0);
  }
  return std::clamp(1.0 - c.reset_fade_out, 0.0, 1.0);
}

void Session::mark_lap_dirty(size_t i) {
  if (i >= cars_.size()) return;
  cars_[i].lap_dirty = true;
  cars_[i].race_dirty = true;
}

Session::Snapshot Session::snapshot() const {
  return {cars_, progress_, resets_, clock_};
}

void Session::restore(const Snapshot& s) {
  if (s.cars.size() != cars_.size()) return;
  cars_ = s.cars;
  progress_ = s.progress;
  resets_ = s.resets;
  clock_ = s.clock;
}

void Session::step(double dt, std::span<const CarSample> s) {
  const double t0 = clock_;
  clock_ += dt;
  resets_.clear();
  const size_t n = std::min(s.size(), cars_.size());
  for (size_t i = 0; i < n; ++i) {
    Car& c = cars_[i];
    const CarSample& q = s[i];
    progress_[i] = q.progress;
    if (t0 + dt <= 0) continue;  // nothing counts before the signal
    if (c.finish < 0) c.distance += std::hypot(q.x1 - q.x0, q.z1 - q.z0);
    // Crossings, at the fraction of the step where the plane was met.
    const double f = start_.crossing(q.x0, q.z0, q.x1, q.z1);
    if (f >= 0 && c.finish < 0) {
      const double t = std::max(0.0, t0 + f * dt);
      if (circuit_) {
        if (c.crossings > 0) {
          const double lap = t - c.last_cross;
          c.laps.push_back(lap);
          if (c.best_lap < 0 || lap < c.best_lap) c.best_lap = lap;
        }
        ++c.crossings;
        c.last_cross = t;
        c.lap_dirty = false;  // fresh lap starts clean; race_dirty remains set if ever rewound
        if (c.crossings >= laps_ + 1) c.finish = t;
      } else ++c.crossings;
    }
    if (!circuit_ && finish_ && c.finish < 0 && !c.finish_held) {
      const double g = finish_->crossing(q.x0, q.z0, q.x1, q.z1);
      if (g >= 0) c.finish = std::max(0.0, t0 + g * dt);
    }
    // The interim automatic resets (every car).
    if (c.reset_fade_in_timer > 0.0) c.reset_fade_in_timer = std::max(0.0, c.reset_fade_in_timer - dt);
    if (o_.resets && c.finish < 0) {
      const double mph = q.speed / 0.44704;
      if (mph >= 5.0 || c.distance > 15.0) c.has_launched = true;
      const bool start_grace = (i == o_.player_index) && (clock_ < o_.start_launch_grace) && !c.has_launched;
      c.rolled = q.up_y < 0.5 ? c.rolled + dt : 0;
      c.wrong = (q.heading_cos < 0.342 && mph > 40) ? c.wrong + dt : 0;
      c.slow = (!start_grace && mph < 5) ? c.slow + dt : 0;
      c.off5 = q.off_route > o_.off_route_near ? c.off5 + dt : 0;
      c.off15 = q.off_route > o_.off_route_far ? c.off15 + dt : 0;
      // Track clean 2 s fade-out leading into any active reset trigger.
      double min_rem = 1e9;
      if (c.rolled > 0.0) min_rem = std::min(min_rem, std::max(0.0, o_.rollover_time - c.rolled));
      if (c.wrong > 0.0) min_rem = std::min(min_rem, std::max(0.0, 3.0 - c.wrong));
      if (c.slow > 0.0) min_rem = std::min(min_rem, std::max(0.0, 3.0 - c.slow));
      if (c.off15 > 0.0) min_rem = std::min(min_rem, std::max(0.0, 3.0 - c.off15));
      if (c.off5 > 0.0) min_rem = std::min(min_rem, std::max(0.0, 6.0 - c.off5));
      if (o_.reset_fade_out > 1e-6 && min_rem < o_.reset_fade_out) {
        c.reset_fade_out = std::clamp(1.0 - min_rem / o_.reset_fade_out, 0.0, 1.0);
      } else {
        c.reset_fade_out = 0.0;
      }
      c.reason = c.rolled >= o_.rollover_time ? "rolled over"
               : c.wrong >= 3                 ? "wrong way"
               : c.slow >= 3                  ? "no progress"
               : c.off15 >= 3                 ? "15 m off"
               : c.off5 >= 6                  ? "5 m off"
                                              : "";
      if (*c.reason) resets_.push_back(i);
    } else {
      c.reset_fade_out = 0.0;
    }
  }
  const auto ord = order();
  for (size_t k = 0; k < ord.size(); ++k) cars_[ord[k]].place = int(k) + 1;
}

std::vector<size_t> Session::order() const {
  std::vector<size_t> o(cars_.size());
  std::iota(o.begin(), o.end(), size_t(0));
  std::stable_sort(o.begin(), o.end(), [&](size_t a, size_t b) {
    const Car &A = cars_[a], &B = cars_[b];
    const bool fa = A.finish >= 0, fb = B.finish >= 0;
    if (fa != fb) return fa;
    if (fa) return A.finish < B.finish;
    if (circuit_ && A.crossings != B.crossings) return A.crossings > B.crossings;
    return progress_[a] > progress_[b];
  });
  return o;
}

void plan_lanes(const drive::RacingLine& line, std::span<const double> progress, std::span<const float> cross,
                std::span<const float> speed, std::span<const LaneParams> params, size_t ai, std::span<float> lane,
                double dt, double rate, std::span<float> cap, std::span<const TrafficObstacle> traffic) {
  const size_t n = std::min({progress.size(), cross.size(), speed.size()});
  constexpr float kWidth = 2.05f;  // a car's width, m
  for (size_t i = 0; i < ai && i < n && i < lane.size(); ++i) {
    const LaneParams& p = params[std::min(i, params.size() - 1)];
    const float aggro = p.aggression > 1.0f ? std::clamp(p.aggression * 0.1f, 0.0f, 1.0f)
                                            : std::clamp(p.aggression, 0.0f, 1.0f);
    const float sep = kWidth + p.car_clearance + 0.25f;
    // Look further ahead at speed so overtaking initiates ~0.9 s before closing
    // rather than stacking in a conga line behind kFollowRange; Passing_AggressionLevel
    // scales overtaking range, speed-margin threshold, gap willingness, and lane rate.
    const double base_pass_range = std::max<double>(p.trailing, std::min(32.0, 0.9 * std::max(0.0f, speed[i])));
    const double pass_range = base_pass_range * (0.75 + 0.65 * double(aggro));
    const float speed_margin = (1.0f - aggro) * 2.5f;
    size_t block = n;
    double best = 1e30;
    for (size_t j = 0; j < n; ++j) {
      if (j == i) continue;
      const double gap = progress[j] - progress[i];
      if (gap <= 0 || gap > pass_range || gap >= best) continue;
      if (std::fabs(cross[j] - cross[i]) > sep && std::fabs(cross[j] - lane[i]) > sep) continue;
      // Trigger pass when faster by aggression margin OR close behind.
      if (speed[i] <= speed[j] + speed_margin && gap > p.trailing * (0.7 + 0.6 * double(aggro)) + 2.0) continue;
      block = j, best = gap;
    }
    double right = line.room(progress[i], true), left = line.room(progress[i], false);
    if (right < 1e8) {
      // Check room ahead as well so overtaking never pushes a car into a
      // narrowing barrier 15-35 m down the road.
      for (double da : {10.0, 20.0, 35.0}) {
        const double pa = std::min(line.length(), progress[i] + da);
        right = std::min(right, line.room(pa, true));
        left = std::min(left, line.room(pa, false));
      }
    }
    const double edge_pad = std::max<double>(p.edge_clearance, 0.35);
    const float min_lane = right < 1e8 ? -float(std::max(0.0, left - edge_pad)) : -3.3f;
    const float max_lane = right < 1e8 ? float(std::max(0.0, right - edge_pad)) : 3.3f;
    float target = 0.0f;
    if (block < n) {
      // Check how much clear room exists on each side of the blocking car.
      const float pass_r =
          right < 1e8 ? std::min(3.3f, std::max(sep + 0.35f, float(0.5 * right + p.car_clearance))) : sep + 0.35f;
      const float pass_l =
          right < 1e8 ? std::min(3.3f, std::max(sep + 0.35f, float(0.5 * left + p.car_clearance))) : sep + 0.35f;
      const float cand_right = std::clamp(cross[block] + pass_r, min_lane, max_lane);
      const float cand_left = std::clamp(cross[block] - pass_l, min_lane, max_lane);
      const float room_r = cand_right - cross[block];
      const float room_l = cross[block] - cand_left;
      const float min_pass_room = sep * (0.90f - 0.25f * aggro);
      if (room_r >= min_pass_room || room_l >= min_pass_room) {
        // Keep current side if it has enough clearance; else take the wider side.
        float side = lane[i] > 0.35f ? 1.0f : lane[i] < -0.35f ? -1.0f : 0.0f;
        if (side > 0 && room_r < sep * 0.75f && room_l > room_r) side = -1.0f;
        else if (side < 0 && room_l < sep * 0.75f && room_r > room_l) side = 1.0f;
        if (side == 0) side = room_r >= room_l ? 1.0f : -1.0f;
        target = side > 0 ? cand_right : cand_left;
      }
    }
    // Street/Nemesis live traffic avoidance: detect slower or oncoming traffic
    // ahead along the racing line and shift cleanly onto the wider side.
    bool dodging_traffic = false;
    const float tsep = kWidth + std::max(p.car_clearance, 0.35f) + 0.55f;
    if (!traffic.empty()) {
      size_t tblock = traffic.size();
      double tbest = 1e30;
      for (size_t t = 0; t < traffic.size(); ++t) {
        const auto& obs = traffic[t];
        const double gap = obs.progress - progress[i];
        const double look = obs.oncoming ? std::max(70.0, (std::max(0.0f, speed[i]) + obs.speed) * 2.5)
                                         : std::max(48.0, std::max(0.0f, speed[i]) * 2.5);
        if (gap < -3.0 || gap > look || gap >= tbest) continue;
        if (std::fabs(obs.cross - target) < tsep || std::fabs(obs.cross - cross[i]) < tsep ||
            std::fabs(obs.cross - lane[i]) < tsep) {
          tblock = t;
          tbest = gap;
        }
      }
      if (tblock < traffic.size()) {
        const auto& obs = traffic[tblock];
        const float cand_r = std::clamp(obs.cross + tsep + 0.25f, min_lane, max_lane);
        const float cand_l = std::clamp(obs.cross - tsep - 0.25f, min_lane, max_lane);
        const float room_r = cand_r - obs.cross;
        const float room_l = obs.cross - cand_l;
        if (std::fabs(room_r - room_l) < 0.15f) target = obs.cross >= 0.0f ? cand_l : cand_r;
        else target = room_r > room_l ? cand_r : cand_l;
        dodging_traffic = true;
      }
    }
    // Check all cars alongside (within 5.8 m longitudinally) so cars never merge
    // sideways into a neighbour or rub doors through a corner.
    for (size_t j = 0; j < n; ++j) {
      if (j == i || std::fabs(progress[j] - progress[i]) > 5.8) continue;
      const float d_now = cross[i] - cross[j];
      const float d_tgt = target - cross[j];
      if (std::fabs(d_now) > sep + 0.2f && std::fabs(d_tgt) > sep) continue;
      const float side = d_now > 0.05f ? 1.0f : d_now < -0.05f ? -1.0f : (i < j ? 1.0f : -1.0f);
      const float push = cross[j] + side * (sep + 0.25f);
      if (side > 0) target = std::max(target, push);
      else target = std::min(target, push);
    }
    target = std::clamp(target, min_lane, max_lane);
    const double aggro_rate = rate * (0.75 + 0.55 * double(aggro));
    const double eff_rate = dodging_traffic ? std::max(aggro_rate, 4.8) : aggro_rate;
    const float step = float(eff_rate * dt);
    lane[i] += std::clamp(target - lane[i], -step, step);
    if (i < cap.size()) {
      cap[i] = 1e9f;
      // Only cap speed behind a car that still blocks both our current lateral
      // position and our chosen target lane, so an active overtake is not
      // brake-checked.
      constexpr float kFollowRange = 36, kMinGap = 5.0f, kBrake = 15.0f;
      for (size_t j = 0; j < n; ++j) {
        if (j == i) continue;
        const double gap = progress[j] - progress[i];
        if (gap <= 0 || gap > kFollowRange) continue;
        const bool in_curr = std::fabs(cross[i] - cross[j]) < kWidth + p.car_clearance;
        const bool in_lane = std::fabs(lane[i] - cross[j]) < kWidth + p.car_clearance;
        const bool in_tgt = std::fabs(target - cross[j]) < kWidth + p.car_clearance;
        if (!(in_curr && (in_lane || (in_tgt && gap < 10.0)))) continue;
        const float vj = std::max(0.0f, speed[j]);
        const float room = std::max(0.0f, float(gap) - kMinGap);
        cap[i] = std::min(cap[i], std::sqrt(vj * vj + 2 * kBrake * room));
      }
      if (block < n && std::fabs(cross[i] - cross[block]) < kWidth + p.car_clearance &&
          std::fabs(lane[i] - cross[block]) < kWidth + p.car_clearance && best < 9.0) {
        cap[i] = std::min(cap[i], std::max(0.0f, speed[block]) + 0.5f * std::max(0.0f, float(best) - 5.0f));
      }
      // Brake smoothly if a live traffic car still blocks our current path.
      for (const auto& obs : traffic) {
        const double gap = obs.progress - progress[i];
        const double range = obs.oncoming ? 55.0 : 42.0;
        if (gap <= 0 || gap > range) continue;
        const bool in_curr = std::fabs(cross[i] - obs.cross) < kWidth + 0.45f;
        const bool in_lane = std::fabs(lane[i] - obs.cross) < kWidth + 0.45f;
        const bool in_tgt = std::fabs(target - obs.cross) < kWidth + 0.45f;
        if (!(in_curr && (in_lane || (in_tgt && gap < 14.0)))) continue;
        const float vt = obs.oncoming ? 0.0f : std::max(0.0f, obs.speed);
        const float room = std::max(0.0f, float(gap) - 6.5f);
        cap[i] = std::min(cap[i], std::sqrt(vt * vt + 2 * 16.0f * room));
      }
    }
  }
}

}  // namespace race
