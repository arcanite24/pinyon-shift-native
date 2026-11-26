#include "race/session.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

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
  c.collisions_off = false;
  c.ai = {};
  c.no_collide_until = clock_ + o_.post_reset_non_collide;
}

void apply_hand_of_god(const HandOfGodReset& h, Session::Options& o) {
  o.reset_fade_out = h.fade_out;
  o.reset_fade_in = h.fade_in;
  o.post_reset_non_collide = h.post_reset_non_collide;
  o.collisions_off = h.collisions_off;
  o.facing_speed_mph = h.facing_speed_mph;
  o.facing_cos = h.facing_cos;
  o.facing_time = h.facing_time;
  o.progress_mph = h.progress_mph;
  o.progress_time = h.progress_time;
  o.off_route_near = h.off_track_m0;
  o.off_route_near_time = h.off_track_time0;
  o.off_route_far = h.off_track_m1;
  o.off_route_far_time = h.off_track_time1;
}

// AIRacing.xml MetaAIDriver/ResetParameters (SPEC-RACE-AI; decoded from
// the AI driver's per-frame check, SPEC_QUESTIONS Q232):
// - the cancel: a car more than CancelResetIfMovedThisFar_m (horizontally)
//   from the stuck-distance anchor restarts every timer and anchor. The
//   anchor moves whenever the car leaves its stuck distance, so only a
//   jump (a reset or a teleport) reaches it;
// - the waypoints: one timer since the car was last N3 waypoints past its
//   anchor waypoint. While it is under N1 waypoints past, the timer fires
//   after MinTime1; under N2, MinTime2; under N3, MinTime3;
// - the stuck distance: within Distance_m of the anchor for its MinTime;
// - slower than Speed_mpers for its MinTime; upside down (up.y below the
//   rollover threshold) and slower than its speed for its MinTime;
// - ResetIfFellThisLow: the world height, at once.
// Each timer fires once its time is past (>) the limit. Interim: the line's
// points stand in for the route's waypoints, the speed is the forward
// speed (the original's is horizontal), and the original's upside-down
// rule also needs at most two wheels on something (not modelled).
const char* Session::step_ai_rules(Car& c, const CarSample& q, double dt, const AiResetRules& r) {
  Car::AiTimers& a = c.ai;
  if (a.near_set && std::hypot(q.x1 - a.near_x, q.z1 - a.near_z) > r.cancel_moved_m) a = {};
  const double speed = std::fabs(q.speed);
  if (q.waypoint >= 0) {
    if (a.waypoint_anchor < 0) a.waypoint_anchor = q.waypoint;
    const double n = std::trunc(std::fabs(q.waypoint - a.waypoint_anchor));
    a.waypoint_tier = -1;
    for (int k = 0; k < 3; ++k) {
      if (n < r.stuck_waypoints[size_t(k)].count) {
        a.waypoint_tier = k;
        break;
      }
    }
    if (a.waypoint_tier < 0) {
      a.waypoint_anchor = q.waypoint;
      a.waypoint_t = 0;
    } else {
      a.waypoint_t += dt;
    }
  }
  if (!a.near_set || std::hypot(q.x1 - a.near_x, q.z1 - a.near_z) >= r.stuck_distance_m) {
    a.near_x = q.x1, a.near_z = q.z1, a.near_set = true;
    a.near_t = 0;
  } else {
    a.near_t += dt;
  }
  a.slow_t = speed < r.slow_speed ? a.slow_t + dt : 0;
  a.upside_down_t = (q.up_y < r.upside_down_up_y && speed < r.upside_down_speed) ? a.upside_down_t + dt : 0;
  if (a.waypoint_tier >= 0 && a.waypoint_t > r.stuck_waypoints[size_t(a.waypoint_tier)].min_time)
    return "stuck on the line";
  if (a.near_t > r.stuck_distance_time) return "stuck in place";
  if (a.slow_t > r.slow_time) return "too slow";
  if (a.upside_down_t > r.upside_down_time) return "upside down";
  if (q.y < r.fell_low) return "fell";
  return "";
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
    // The automatic resets: the player (and, without AI rules, every car)
    // by the Hand of God triggers; the AI by its own ResetParameters.
    if (c.reset_fade_in_timer > 0.0) c.reset_fade_in_timer = std::max(0.0, c.reset_fade_in_timer - dt);
    const bool player = i == o_.player_index;
    const bool by_ai_rules = !player && o_.ai_rules.has_value();
    const bool by_hand_of_god = player || (!o_.ai_rules && o_.ai_hand_of_god);
    if (o_.resets && c.finish < 0 && (by_ai_rules || by_hand_of_god)) {
      const double mph = q.speed / 0.44704;
      if (mph >= 5.0 || c.distance > 15.0) c.has_launched = true;
      // Seconds left on the trigger nearest to firing (1e9: none running).
      double min_rem = 1e9;
      auto left = [&](double t, double limit) {
        if (t > 0.0) min_rem = std::min(min_rem, std::max(0.0, limit - t));
      };
      if (by_ai_rules) {
        c.reason = step_ai_rules(c, q, dt, *o_.ai_rules);
        const AiResetRules& r = *o_.ai_rules;
        if (c.ai.waypoint_tier >= 0)
          left(c.ai.waypoint_t, r.stuck_waypoints[size_t(c.ai.waypoint_tier)].min_time);
        left(c.ai.near_t, r.stuck_distance_time);
        left(c.ai.slow_t, r.slow_time);
        left(c.ai.upside_down_t, r.upside_down_time);
        if (*c.reason) min_rem = 0.0;
      } else {
        const bool start_grace = player && (clock_ < o_.start_launch_grace) && !c.has_launched;
        c.rolled = q.up_y < 0.5 ? c.rolled + dt : 0;
        c.wrong = (q.heading_cos < o_.facing_cos && mph > o_.facing_speed_mph) ? c.wrong + dt : 0;
        c.slow = (!start_grace && mph < o_.progress_mph) ? c.slow + dt : 0;
        c.off5 = q.off_route > o_.off_route_near ? c.off5 + dt : 0;
        c.off15 = q.off_route > o_.off_route_far ? c.off15 + dt : 0;
        left(c.rolled, o_.rollover_time);
        left(c.wrong, o_.facing_time);
        left(c.slow, o_.progress_time);
        left(c.off15, o_.off_route_far_time);
        left(c.off5, o_.off_route_near_time);
        c.reason = c.rolled >= o_.rollover_time        ? "rolled over"
                 : c.wrong >= o_.facing_time           ? "wrong way"
                 : c.slow >= o_.progress_time          ? "no progress"
                 : c.off15 >= o_.off_route_far_time    ? "far off route"
                 : c.off5 >= o_.off_route_near_time    ? "off route"
                                                       : "";
      }
      // The fade-out leading into the trigger nearest to firing.
      if (o_.reset_fade_out > 1e-6 && min_rem < o_.reset_fade_out) {
        c.reset_fade_out = std::clamp(1.0 - min_rem / o_.reset_fade_out, 0.0, 1.0);
      } else {
        c.reset_fade_out = 0.0;
      }
      // CollisionsOffTime (inferred from the name, not traced): the car
      // stops touching other cars over the last CollisionsOffTime before
      // it is put back, the window the fade-out covers; contact returns
      // if the trigger clears first.
      c.collisions_off = min_rem < o_.collisions_off;
      if (*c.reason) resets_.push_back(i);
    } else {
      c.reset_fade_out = 0.0;
      c.collisions_off = false;
    }
  }
  const auto ord = order();
  for (size_t k = 0; k < ord.size(); ++k) cars_[ord[k]].place = int(k) + 1;
  update_class_places(ord);
}

void Session::update_class_places(const std::vector<size_t>& ord) {
  std::vector<std::pair<int, int>> counts;  // (car_class, next_place)
  for (size_t idx : ord) {
    const int cls = cars_[idx].car_class;
    int next_p = 1;
    bool found = false;
    for (auto& kv : counts) {
      if (kv.first == cls) {
        next_p = ++kv.second;
        found = true;
        break;
      }
    }
    if (!found) counts.emplace_back(cls, 1);
    cars_[idx].class_place = next_p;
  }
}

void Session::set_car_class(size_t i, int car_class) {
  if (i >= cars_.size()) return;
  cars_[i].car_class = car_class;
  update_class_places(order());
}

void Session::set_car_classes(std::span<const int> classes) {
  const size_t n = std::min(classes.size(), cars_.size());
  for (size_t i = 0; i < n; ++i) cars_[i].car_class = classes[i];
  update_class_places(order());
}

int Session::car_class(size_t i) const { return i < cars_.size() ? cars_[i].car_class : 0; }

int Session::class_place(size_t car_idx) const {
  if (car_idx >= cars_.size()) return 0;
  const int target_cls = cars_[car_idx].car_class;
  const auto ord = order();
  int rank = 0;
  for (size_t idx : ord) {
    if (cars_[idx].car_class != target_cls) continue;
    ++rank;
    if (idx == car_idx) return rank;
  }
  return 0;
}

std::vector<size_t> Session::class_order(int car_class) const {
  std::vector<size_t> out;
  for (size_t idx : order()) {
    if (cars_[idx].car_class == car_class) out.push_back(idx);
  }
  return out;
}

bool Session::is_multi_class() const {
  if (cars_.size() < 2) return false;
  const int first_cls = cars_.front().car_class;
  for (size_t i = 1; i < cars_.size(); ++i) {
    if (cars_[i].car_class != first_cls) return true;
  }
  return false;
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

}  // namespace race
