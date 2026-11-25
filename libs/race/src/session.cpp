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

// AIRacing.xml MetaAIDriver/ResetParameters (SPEC-RACE-AI, Routes with no
// AI line; the rule's code is not decoded, so how each condition is
// measured is read from the attribute names, interim):
// - stuck within N_k waypoints for MinTime_k: the car has not moved N_k
//   points along the line from where the timer started;
// - stuck within the distance for its MinTime: it has stayed within that
//   many metres of where the timer started;
// - slower than Speed_mpers for MinTime; upside down (up.y below the
//   rollover threshold) and slower than its speed for its MinTime;
// - fell below ResetIfFellThisLow: the world height, at once;
// - CancelResetIfMovedThisFar_m: once the car is that far (horizontally)
//   from where the running timers started, they all start again.
const char* Session::step_ai_rules(Car& c, const CarSample& q, double dt, const AiResetRules& r) {
  Car::AiTimers& a = c.ai;
  if (q.y < r.fell_low) return "fell";
  const bool running = a.waypoint_t[0] > 0 || a.waypoint_t[1] > 0 || a.waypoint_t[2] > 0 || a.near_t > 0 ||
                       a.slow_t > 0 || a.upside_down_t > 0;
  if (!running || !a.moved_set) {
    a.moved_x = q.x1, a.moved_z = q.z1, a.moved_set = true;
  } else if (std::hypot(q.x1 - a.moved_x, q.z1 - a.moved_z) > r.cancel_moved_m) {
    a = {};
    a.moved_x = q.x1, a.moved_z = q.z1, a.moved_set = true;
  }
  const double speed = std::fabs(q.speed);
  for (int k = 0; k < 3; ++k) {
    const auto& rule = r.stuck_waypoints[size_t(k)];
    if (q.waypoint < 0 || rule.count <= 0) {
      a.waypoint_t[k] = 0;
      continue;
    }
    if (a.waypoint_anchor[k] < 0 || std::fabs(q.waypoint - a.waypoint_anchor[k]) >= rule.count) {
      a.waypoint_anchor[k] = q.waypoint;
      a.waypoint_t[k] = 0;
    } else {
      a.waypoint_t[k] += dt;
    }
  }
  if (!a.near_set || std::hypot(q.x1 - a.near_x, q.z1 - a.near_z) > r.stuck_distance_m) {
    a.near_x = q.x1, a.near_z = q.z1, a.near_set = true;
    a.near_t = 0;
  } else {
    a.near_t += dt;
  }
  a.slow_t = speed < r.slow_speed ? a.slow_t + dt : 0;
  a.upside_down_t = (q.up_y < r.upside_down_up_y && speed < r.upside_down_speed) ? a.upside_down_t + dt : 0;
  if (a.upside_down_t >= r.upside_down_time) return "upside down";
  for (int k = 0; k < 3; ++k)
    if (a.waypoint_t[k] > 0 && a.waypoint_t[k] >= r.stuck_waypoints[size_t(k)].min_time) return "stuck on the line";
  if (a.near_t >= r.stuck_distance_time) return "stuck in place";
  if (a.slow_t >= r.slow_time) return "too slow";
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
        for (int k = 0; k < 3; ++k) left(c.ai.waypoint_t[k], r.stuck_waypoints[size_t(k)].min_time);
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
