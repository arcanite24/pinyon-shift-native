#include "race/dls.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace race {
namespace {

constexpr int kHalfSlots = 100;   // slots -100..100, 0.5 m apart
constexpr double kSlot = 0.5;     // m
constexpr double kHorizon = 2.25; // s, the prediction's length
constexpr double kSideBySide = 3; // m along the line: alongside now

// The car's lateral place t seconds ahead: moving from where it is to its
// lane over a second (interim, Q123: the title predicts its own path).
double lateral_at(const DlsCar& c, float lane, double t) {
  return c.cross + (lane - c.cross) * std::clamp(t / 1.0, 0.0, 1.0);
}

double extra_width(const DlsSettings& s, DlsCar::Kind k) {
  switch (k) {
    case DlsCar::Kind::kHuman: return s.extra_width_human;
    case DlsCar::Kind::kTraffic: return s.extra_width_traffic;
    default: return s.extra_width_ai;
  }
}

double room_or(const drive::RacingLine& line, double p, bool right) {
  const double r = line.room(std::clamp(p, 0.0, double(line.length())), right);
  return r < 1e8 ? r : 3.3;
}

}  // namespace

DlsMeeting dls_predict(const DlsCar& self, float self_lane, const DlsCar& other, double horizon, bool along_only) {
  DlsMeeting m;
  const double r_sum = std::hypot(self.half_width, self.half_length) + std::hypot(other.half_width, other.half_length);
  const double hl = self.half_length + other.half_length, hw = self.half_width + other.half_width;
  constexpr double kDt = 1.0 / 30;
  for (int k = 0; k * kDt <= horizon + 1e-9; ++k) {
    const double t = k * kDt;
    const double ds = (other.progress + other.speed * t) - (self.progress + self.speed * t);
    if (std::fabs(ds) > 12.0) continue;  // the step is skipped while the cars are far apart along the line
    const double dl = along_only ? 0.0 : other.cross - lateral_at(self, self_lane, t);
    const double grow = 1 + 0.1 * std::clamp(t / 2, 0.0, 1.0);
    if (std::hypot(ds, dl) < r_sum * grow && std::fabs(ds) < hl && std::fabs(dl) < hw) {
      m.valid = m.hit = true, m.t = t, m.miss = 0;
      return m;
    }
    const double gap = std::hypot(std::max(0.0, std::fabs(ds) - hl), std::max(0.0, std::fabs(dl) - hw));
    if (std::hypot(ds, dl) < 32 && gap < m.miss) m.valid = true, m.t = t, m.miss = gap;
  }
  return m;
}

double dls_search(const drive::RacingLine& line, std::span<const DlsCar> cars, size_t i, float self_lane,
                  size_t target, const DlsMeeting& meet, uint8_t current_side, const DlsSettings& st, float& lane,
                  uint8_t& side) {
  const DlsCar& me = cars[i];
  const double t_meet = meet.t;
  const double pred = lateral_at(me, self_lane, t_meet);
  const double v = std::max(0.0f, me.speed);
  const double d_coll = v * t_meet;
  const double s_coll = me.progress + d_coll;
  // The road's limits for the car's centre at the meeting point, relative to
  // its predicted place (the line's room is for a 1 m half width).
  const double hw_s = me.half_width;
  const double right = room_or(line, s_coll, true) + 1.0 - hw_s - pred;
  const double left = -(room_or(line, s_coll, false) + 1.0 - hw_s) - pred;
  const int out_l = int(std::floor((left + 0.1) / kSlot)), edge_l = std::max(int(std::ceil(left / kSlot)), out_l);
  const int out_r = int(std::ceil((right - 0.1) / kSlot)), edge_r = std::min(int(std::floor(right / kSlot)), out_r);
  constexpr int kN = 2 * kHalfSlots + 1;
  std::array<bool, kN> car{};
  std::array<double, kN> ttc;
  ttc.fill(1000.0);
  auto cover = [&](int from, int to, double t) {
    for (int j = std::max(from, -kHalfSlots); j <= std::min(to, kHalfSlots); ++j) {
      car[size_t(j + kHalfSlots)] = true;
      ttc[size_t(j + kHalfSlots)] = std::min(ttc[size_t(j + kHalfSlots)], t);
    }
  };
  for (size_t m = 0; m < cars.size(); ++m) {
    if (m == i) continue;
    const DlsCar& o = cars[m];
    const double e = extra_width(st, o.kind);
    if (std::fabs(o.progress - me.progress) < kSideBySide) {
      // Alongside now: every slot from its near edge onwards.
      if (o.cross > me.cross) cover(int(std::floor((o.cross - o.half_width - hw_s - e - pred) / kSlot)), kHalfSlots, 0);
      else cover(-kHalfSlots, int(std::ceil((o.cross + o.half_width + hw_s + e - pred) / kSlot)), 0);
      continue;
    }
    const DlsMeeting mm = m == target ? meet : dls_predict(me, self_lane, o);
    if (!mm.valid || mm.t > 10) continue;
    const double ds = (o.progress + o.speed * mm.t) - (me.progress + me.speed * mm.t);
    if (std::fabs(ds) - o.half_length > 1.5 * me.half_length) continue;  // not near it then (interim reading, Q123)
    const double mn = o.cross - o.half_width - pred, mx = o.cross + o.half_width - pred;
    cover(int(std::floor((mn - hw_s - e) / kSlot)), int(std::ceil((mx + hw_s + e) / kSlot)), mm.t);
  }
  const DlsCar& tg = cars[target];
  const int k_star = int(std::lround((tg.cross - pred) / kSlot));
  const bool head_on = tg.speed < 0 && me.speed > 0;
  const double w_off = std::max(1.0, v * st.off_line_cost / std::max(d_coll, 2.0));
  auto g = [&](int j) { return std::max(kHorizon - ttc[size_t(std::clamp(j, -kHalfSlots, kHalfSlots) + kHalfSlots)], 0.0); };
  double best = 1e7;
  int best_j = 0;
  for (int j = -kHalfSlots + 1; j <= kHalfSlots - 1; ++j) {
    const double off = j * kSlot;
    double c = std::fabs(off) * w_off;
    if (j < out_l || j > out_r) c += st.out_of_bounds_cost;
    else if (j <= edge_l || j >= edge_r) c += st.off_track_cost;
    if (car[size_t(j + kHalfSlots)]) c += st.car_cost;
    c += st.time_to_collision_cost * (g(j) + 0.5 * (g(j - 1) + g(j + 1)));
    if ((current_side == 1 && j > k_star) || (current_side == 2 && j < k_star)) c += st.change_pass_sides_cost;
    if (head_on && ((st.legal_side == 2 && j < k_star) || (st.legal_side == 1 && j > k_star)))
      c += st.head_on_wrong_side_cost;
    if (c < best) best = c, best_j = j;
  }
  lane = float(pred + best_j * kSlot);
  side = best_j < k_star ? 1 : 2;
  return best;
}

void dls_update(const drive::RacingLine& line, std::span<const DlsCar> cars, size_t ai,
                std::span<const LaneParams> params, const DlsSettings& st, double dt, std::span<DlsState> state,
                std::span<float> lane, std::span<float> cap) {
  const size_t n = cars.size();
  for (size_t i = 0; i < ai && i < n && i < lane.size() && i < state.size(); ++i) {
    const DlsCar& me = cars[i];
    DlsState& s = state[i];
    const LaneParams& p = params.empty() ? LaneParams{} : params[std::min(i, params.size() - 1)];
    const double aggro = std::clamp(double(p.aggression > 1 ? p.aggression * 0.1f : p.aggression), 0.0, 1.0);
    // The target: the not-yet-passed car met first (a hit or a miss under
    // 0.2 m) within the horizon, among the cars within 250 m.
    const double rear_margin = 0.5 + 0.2 * aggro;
    auto passed = [&](const DlsCar& o) {
      return o.progress - me.progress < -(me.half_length + o.half_length + rear_margin);
    };
    int target = -1;
    DlsMeeting meet;
    // The car being passed stays the target until the car is past it or no
    // longer closing on it (interim, Q123). Traffic cars have no stable
    // index from step to step, so a traffic target is chosen afresh.
    const bool kept = s.target >= 0 && size_t(s.target) < n && size_t(s.target) != i &&
                      cars[size_t(s.target)].kind != DlsCar::Kind::kTraffic;
    if (kept && !passed(cars[size_t(s.target)])) {
      const DlsCar& o = cars[size_t(s.target)];
      const DlsMeeting mm = std::fabs(o.progress - me.progress) < me.half_length + o.half_length
                                ? DlsMeeting{true, true, 0.0, 0.0}
                                : dls_predict(me, lane[i], o, kHorizon, true);
      if (mm.valid && (mm.hit || mm.miss < 0.2)) target = s.target, meet = mm;
    }
    for (size_t m = 0; m < n && target < 0; ++m) {
      if (m == i) continue;
      const DlsCar& o = cars[m];
      const double gap = o.progress - me.progress;
      if (std::fabs(gap) > 250) continue;
      if (passed(o)) continue;
      const DlsMeeting mm = dls_predict(me, lane[i], o, kHorizon);
      if (!mm.valid && st.ignore_cars_not_approached) continue;
      if (!(mm.hit || mm.miss < 0.2)) continue;
      if (target < 0 || mm.t < meet.t) target = int(m), meet = mm;
    }
    if (target != s.target) {
      if (s.following && kept && target < 0) {
        // Keep trailing until it is out of the way (20 m ahead) or gone.
        const DlsCar& o = cars[size_t(s.target)];
        if (o.progress - me.progress < 20 && o.progress > me.progress) target = s.target;
      }
      if (target != s.target) {
        s.target = target, s.side = 0, s.timer = 0, s.following = false, s.frustration = 0;
      }
    }
    const double v = std::max(0.0f, me.speed);
    if (target < 0) {
      // Nothing to pass: back to the line (interim, Q123), never into a car alongside.
      float goal = 0;
      for (size_t m = 0; m < n; ++m) {
        if (m == i) continue;
        const DlsCar& o = cars[m];
        if (std::fabs(o.progress - me.progress) > me.half_length + o.half_length + 1) continue;
        const float need = me.half_width + o.half_width + float(extra_width(st, o.kind)) + 0.3f;
        if (me.cross <= o.cross) goal = std::min(goal, o.cross - need);  // stay on its left
        else goal = std::max(goal, o.cross + need);                      // or on its right
      }
      s.target_lane = goal;
      s.rate = 1.0f / 25.0f;
    } else {
      // Following: hold the lane, and look for a pass again once a second
      // (interim, Q123: what ends the title's following is not decoded).
      if (s.following) s.target_lane = lane[i];
      s.timer -= float(dt);
      if (s.timer <= 0) {
        float to = lane[i];
        uint8_t side = s.side;
        const double c = dls_search(line, cars, i, lane[i], size_t(target), meet, s.side, st, to, side);
        if (c < st.max_acceptable_cost) {
          const double d = std::max(v * meet.t, 2.0);
          const double road = 0.5 * (room_or(line, me.progress, true) + room_or(line, me.progress, false)) + 1.0;
          s.target_lane = to, s.side = side;
          s.rate = float(std::min(std::fabs(to - lane[i]) / d, 0.2 * road / 2.0));
          s.timer = float(st.update_interval);
          s.following = false, s.frustration = 0;
        } else if (s.following) {
          s.timer = 1.0f;
        } else {
          // No acceptable slot: hold, try again next frame, and grow
          // frustrated: 5 a frame of the title's 30 Hz (interim, Q123).
          s.target_lane = lane[i];
          s.frustration += float(5 * dt * 30);
          s.timer = 0;
          if (s.frustration > 1000) s.following = true, s.timer = 1.0f;
        }
      }
    }
    const float step = float(std::max(0.3, double(s.rate) * v) * dt);
    lane[i] += std::clamp(s.target_lane - lane[i], -step, step);
    if (i < cap.size()) {
      // Trailing (interim, Q123): no closer than a metre to a car still in
      // both the car's path and its lane, 0.75 x TrailingDistance while following.
      cap[i] = 1e9f;
      for (size_t m = 0; m < n; ++m) {
        if (m == i) continue;
        const DlsCar& o = cars[m];
        const double gap = o.progress - me.progress - me.half_length - o.half_length;
        if (o.progress <= me.progress || gap > 40) continue;
        const float wide = me.half_width + o.half_width + 0.3f;
        if (std::fabs(o.cross - me.cross) >= wide || std::fabs(o.cross - lane[i]) >= wide) continue;
        const double keep = s.following && int(m) == s.target ? 0.75 * p.trailing : 1.0;
        const double vo = std::max(0.0f, o.speed);
        cap[i] = std::min(cap[i], float(std::sqrt(vo * vo + 2 * 12.0 * std::max(0.0, gap - keep))));
      }
    }
  }
}

}  // namespace race
