#include "traffic/scripted.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace traffic {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

double wrap_angle(double a) {
  while (a > kPi) a -= 2.0 * kPi;
  while (a < -kPi) a += 2.0 * kPi;
  return a;
}

bool snap_y(const ScriptedCars::GroundFn& ground, double x, double y_hint, double z, bool force, double& out_y) {
  if (!ground) return false;
  double gy = y_hint;
  if (!ground(x, y_hint, z, gy)) return false;
  if (force || std::abs(y_hint) < 1e-3 || std::abs(gy - y_hint) <= 4.0) {
    out_y = gy;
    return true;
  }
  return false;
}

}  // namespace

std::string ScriptedCars::canonical_name(std::string_view name) {
  constexpr std::string_view kSuffix = "_release";
  if (name.size() > kSuffix.size()) {
    const std::string_view tail = name.substr(name.size() - kSuffix.size());
    if (ieq(tail, kSuffix)) return std::string(name.substr(0, name.size() - kSuffix.size()));
  }
  return std::string(name);
}

const ScriptedCar* ScriptedCars::find(std::string_view name) const {
  const std::string want = canonical_name(name);
  for (const auto& c : cars_)
    if (ieq(c.name, want)) return &c;
  return nullptr;
}

ScriptedCar* ScriptedCars::find_mut(std::string_view name) {
  const std::string want = canonical_name(name);
  for (auto& c : cars_)
    if (ieq(c.name, want)) return &c;
  return nullptr;
}

bool ScriptedCars::erase(std::string_view name) {
  const std::string want = canonical_name(name);
  const auto it = std::remove_if(cars_.begin(), cars_.end(), [&](const ScriptedCar& c) { return ieq(c.name, want); });
  if (it == cars_.end()) return false;
  cars_.erase(it, cars_.end());
  return true;
}

void ScriptedCars::clear() { cars_.clear(); }

void ScriptedCars::set_guidance_route(std::vector<P3> waypoints) { guidance_ = std::move(waypoints); }

uint32_t ScriptedCars::spawn(const SpawnParams& p, const GroundFn& ground) {
  const std::string cname = canonical_name(p.name);
  ScriptedCar* slot = find_mut(cname);
  if (!slot) {
    cars_.emplace_back();
    slot = &cars_.back();
    slot->id = next_id_++;
  }
  slot->name = cname;
  slot->car_id = p.car_id;
  slot->car_media = p.car_media;
  slot->paint_rgb = p.paint_rgb;
  slot->position = p.pos;
  slot->heading = wrap_angle(p.heading_rad);
  slot->pitch = 0.0;
  slot->speed = 0.0;
  slot->target_speed = 0.0;
  slot->steer_angle = 0.0;
  slot->released = false;
  slot->braking = true;
  slot->snap_to_ground = p.snap_to_ground;
  slot->ground_snapped = false;
  slot->pace_ahead = p.pace_ahead;
  slot->age = 0.0;
  slot->despawn_after = 0.0;
  slot->road = -1;
  slot->dir = 1;
  slot->s = 0.0;
  slot->lateral = 0.0;
  slot->target_lateral = 2.0;

  double gy = slot->position.y;
  if (snap_y(ground, slot->position.x, slot->position.y, slot->position.z, p.snap_to_ground, gy)) {
    slot->position.y = gy;
    slot->ground_snapped = true;
  }
  return slot->id;
}

void ScriptedCars::attach_to_road(ScriptedCar& car, const Roads& roads) {
  car.road = -1;
  const RoadHit hit = roads.nearest(car.position.x, car.position.z, false, 60.0);
  if (hit.road < 0 || size_t(hit.road) >= roads.roads().size()) return;
  const Road& rd = roads.roads()[size_t(hit.road)];
  if (rd.length <= 1.0) return;

  std::array<double, 2> tan{0.0, 1.0};
  const P3 on_road = roads.point_at(hit.road, hit.s, nullptr, &tan);
  const double fx = std::sin(car.heading), fz = std::cos(car.heading);
  const double align = fx * tan[0] + fz * tan[1];

  car.road = hit.road;
  car.dir = rd.oneway ? 1 : (align >= 0.0 ? 1 : -1);
  car.s = hit.s;
  const std::array<double, 2> tdir{car.dir * tan[0], car.dir * tan[1]};
  const std::array<double, 2> rvec = right_of(tdir[0], tdir[1]);
  car.lateral = (car.position.x - on_road.x) * rvec[0] + (car.position.z - on_road.z) * rvec[1];
  car.target_lateral = 2.0;
}

bool ScriptedCars::release(const ReleaseParams& p, const Roads* roads, const GroundFn& ground) {
  ScriptedCar* car = find_mut(p.name);
  if (!car) return false;
  if (p.use_release_pose) {
    car->position = p.pos;
    car->heading = wrap_angle(p.heading_rad);
    double gy = car->position.y;
    if (snap_y(ground, car->position.x, car->position.y, car->position.z, p.snap_to_ground, gy)) {
      car->position.y = gy;
      car->ground_snapped = true;
    }
  }
  car->released = true;
  car->braking = false;
  car->age = 0.0;
  car->despawn_after = std::max(0.0, p.despawn_time_s);
  car->speed = std::max(0.0, p.initial_speed_mps);
  // Cars released from a standstill (e.g. Diner lot exits at 0-6 mph) accelerate
  // out onto the road at a spirited cruise speed (~26 m/s / ~58 mph).
  car->target_speed = (p.initial_speed_mps >= 12.0) ? p.initial_speed_mps : std::max(26.0, p.initial_speed_mps);
  if (roads) attach_to_road(*car, *roads);
  return true;
}

bool ScriptedCars::advance_junction(ScriptedCar& car, const Roads& roads, double overshoot) {
  const auto& all = roads.roads();
  if (car.road < 0 || size_t(car.road) >= all.size()) return false;
  const Road& cur = all[size_t(car.road)];
  if (cur.nodes.empty()) return false;

  const uint32_t node = (car.dir > 0) ? cur.nodes.back() : cur.nodes.front();
  std::array<double, 2> cur_tan{0.0, 1.0};
  roads.point_at(car.road, car.dir > 0 ? cur.length : 0.0, nullptr, &cur_tan);
  const std::array<double, 2> cur_dir{car.dir * cur_tan[0], car.dir * cur_tan[1]};

  // Find a lookahead target direction along the guidance route if set.
  bool have_goal = false;
  std::array<double, 2> goal_dir{0.0, 0.0};
  if (!guidance_.empty()) {
    size_t nearest_i = 0;
    double best_d2 = 1e30;
    for (size_t i = 0; i < guidance_.size(); ++i) {
      const double dx = guidance_[i].x - car.position.x, dz = guidance_[i].z - car.position.z;
      const double d2 = dx * dx + dz * dz;
      if (d2 < best_d2) {
        best_d2 = d2;
        nearest_i = i;
      }
    }
    size_t target_i = nearest_i;
    while (target_i + 1 < guidance_.size()) {
      const double dx = guidance_[target_i].x - car.position.x, dz = guidance_[target_i].z - car.position.z;
      if (dx * dx + dz * dz >= 60.0 * 60.0) break;
      ++target_i;
    }
    const double gx = guidance_[target_i].x - car.position.x;
    const double gz = guidance_[target_i].z - car.position.z;
    const double gn = std::hypot(gx, gz);
    if (gn > 5.0) {
      goal_dir = {gx / gn, gz / gn};
      have_goal = true;
    }
  }

  int best_road = -1;
  int best_dir = 1;
  double best_s = 0.0;
  double best_score = -1e9;

  for (const auto& [cand_r, cand_ni] : roads.at_node(node)) {
    if (cand_r == car.road || cand_r < 0 || size_t(cand_r) >= all.size()) continue;
    const Road& rd = all[size_t(cand_r)];
    if (rd.length <= 1.0 || rd.nodes.size() < 2) continue;

    auto eval_dir = [&](int d) {
      if (d < 0 && rd.oneway) return;
      if (d > 0 && size_t(cand_ni) + 1 >= rd.nodes.size()) return;
      if (d < 0 && cand_ni <= 0) return;
      const double s0 = rd.s[size_t(cand_ni)];
      std::array<double, 2> tan{0.0, 1.0};
      roads.point_at(cand_r, std::clamp(s0 + d * 2.0, 0.0, rd.length), nullptr, &tan);
      const std::array<double, 2> cdir{d * tan[0], d * tan[1]};
      const double straight = cur_dir[0] * cdir[0] + cur_dir[1] * cdir[1];
      if (straight < -0.5) return;  // avoid U-turns
      double score = straight;
      if (have_goal) {
        const double g_align = goal_dir[0] * cdir[0] + goal_dir[1] * cdir[1];
        score = 0.65 * g_align + 0.35 * straight;
      }
      if (score > best_score) {
        best_score = score;
        best_road = cand_r;
        best_dir = d;
        best_s = s0;
      }
    };
    eval_dir(1);
    eval_dir(-1);
  }

  if (best_road < 0) return false;
  const Road& next_rd = all[size_t(best_road)];
  car.road = best_road;
  car.dir = best_dir;
  car.s = std::clamp(best_s + best_dir * overshoot, 0.0, next_rd.length);
  return true;
}

void ScriptedCars::step(double dt, const Roads* roads, const GroundFn& ground, const StepOptions& opts) {
  if (dt <= 0.0) return;
  dt = std::min(dt, 0.25);

  for (auto& car : cars_) {
    if (!car.released) {
      // Lazily snap parked cars once world collision is streamed in.
      if (!car.ground_snapped && ground) {
        double gy = car.position.y;
        if (snap_y(ground, car.position.x, car.position.y, car.position.z, car.snap_to_ground, gy)) {
          car.position.y = gy;
          car.ground_snapped = true;
        }
      }
      continue;
    }

    car.age += dt;
    double desired = car.target_speed;

    // Gentle pacing for lead cars (e.g. Darius Flynt's 599XX during the opening
    // Viper drive) so they remain visible ahead of the player rather than vanishing
    // kilometres ahead or getting left behind.
    if (car.pace_ahead && opts.has_player) {
      const double pfx = std::sin(opts.player_heading), pfz = std::cos(opts.player_heading);
      const double ahead = (car.position.x - opts.player_pos.x) * pfx + (car.position.z - opts.player_pos.z) * pfz;
      if (ahead > 115.0) {
        desired = std::min(desired, std::max(18.0, opts.player_speed * 0.85));
      } else if (ahead < 40.0) {
        desired = std::max(desired, std::min(62.0, opts.player_speed + 10.0));
      }
    }

    if (roads && car.road < 0) attach_to_road(car, *roads);

    if (roads && car.road >= 0 && size_t(car.road) < roads->roads().size()) {
      const Road& rd = roads->roads()[size_t(car.road)];
      int seg = 0;
      roads->point_at(car.road, car.s, &seg, nullptr);
      if (seg >= 0 && size_t(seg) < rd.radius.size()) {
        const double r = rd.radius[size_t(seg)];
        if (r > 1.0 && r < 2000.0) {
          const double v_curve = std::sqrt(r * 9.0);
          desired = std::min(desired, std::max(10.0, v_curve));
        }
      }
    }

    const double dv = desired - car.speed;
    if (dv >= 0.0) {
      car.speed += std::min(dv, 12.0 * dt);
      car.braking = false;
    } else {
      car.speed += std::max(dv, -16.0 * dt);
      car.braking = dv < -2.0;
    }
    car.speed = std::max(0.0, car.speed);

    const P3 old_pos = car.position;
    const double old_heading = car.heading;
    const double ds = car.speed * dt;

    if (roads && car.road >= 0 && size_t(car.road) < roads->roads().size()) {
      const Road& rd0 = roads->roads()[size_t(car.road)];
      car.s += car.dir * ds;
      if (car.dir > 0 && car.s >= rd0.length) {
        const double over = car.s - rd0.length;
        if (!advance_junction(car, *roads, over)) {
          car.s = rd0.length;
          car.road = -1;  // fell off road end: continue straight
        }
      } else if (car.dir < 0 && car.s <= 0.0) {
        const double over = -car.s;
        if (!advance_junction(car, *roads, over)) {
          car.s = 0.0;
          car.road = -1;
        }
      }
    }

    if (roads && car.road >= 0 && size_t(car.road) < roads->roads().size()) {
      const double lat_rate = std::min(std::max(car.speed * 0.4, 2.0), 5.0);
      const double dlat = car.target_lateral - car.lateral;
      car.lateral += std::clamp(dlat, -lat_rate * dt, lat_rate * dt);

      std::array<double, 2> tan{0.0, 1.0};
      const P3 pt = roads->point_at(car.road, car.s, nullptr, &tan);
      const std::array<double, 2> tdir{car.dir * tan[0], car.dir * tan[1]};
      const std::array<double, 2> rvec = right_of(tdir[0], tdir[1]);
      car.position.x = pt.x + rvec[0] * car.lateral;
      car.position.z = pt.z + rvec[1] * car.lateral;
      car.position.y = pt.y;

      const double mx = car.position.x - old_pos.x, mz = car.position.z - old_pos.z;
      const double target_h = (mx * mx + mz * mz > 1e-4) ? std::atan2(mx, mz) : std::atan2(tdir[0], tdir[1]);
      const double dh = wrap_angle(target_h - old_heading);
      const double max_turn = 2.8 * dt;
      car.heading = wrap_angle(old_heading + std::clamp(dh, -max_turn, max_turn));
      car.steer_angle = std::clamp(dh / std::max(dt, 1e-3) * 0.18, -0.45, 0.45);
    } else {
      car.position.x += std::sin(car.heading) * ds;
      car.position.z += std::cos(car.heading) * ds;
      car.steer_angle = 0.0;
    }

    double gy = car.position.y;
    if (snap_y(ground, car.position.x, car.position.y, car.position.z, false, gy)) {
      car.position.y = gy;
      car.ground_snapped = true;
    }
    const double horiz = std::hypot(car.position.x - old_pos.x, car.position.z - old_pos.z);
    if (horiz > 0.05) {
      const double target_pitch = std::clamp(std::atan2(car.position.y - old_pos.y, horiz), -0.35, 0.35);
      car.pitch += (target_pitch - car.pitch) * std::min(1.0, 10.0 * dt);
    }
  }

  cars_.erase(std::remove_if(cars_.begin(), cars_.end(),
                             [](const ScriptedCar& c) {
                               return c.released && c.despawn_after > 0.0 && c.age >= c.despawn_after;
                             }),
              cars_.end());
}

}  // namespace traffic
