#include "race/session.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace race {

int player_slot(int p, int cars) {
  if (p >= 1 && p <= cars) return p - 1;
  return std::max(0, cars - 1);
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
  c.wrong = c.slow = c.off5 = c.off15 = 0;
  c.no_collide_until = clock_ + 3.0;  // PostResetNonCollideTime
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
        if (c.crossings >= laps_ + 1) c.finish = t;
      } else ++c.crossings;
    }
    if (!circuit_ && finish_ && c.finish < 0) {
      const double g = finish_->crossing(q.x0, q.z0, q.x1, q.z1);
      if (g >= 0) c.finish = std::max(0.0, t0 + g * dt);
    }
    // The interim automatic resets (every car).
    if (o_.resets) {
      const double mph = q.speed / 0.44704;
      c.wrong = (q.heading_cos < 0.342 && mph > 40) ? c.wrong + dt : 0;
      c.slow = mph < 5 ? c.slow + dt : 0;
      c.off5 = q.off_route > 5 ? c.off5 + dt : 0;
      c.off15 = q.off_route > 15 ? c.off15 + dt : 0;
      if (c.wrong >= 3 || c.slow >= 3 || c.off5 >= 6 || c.off15 >= 3) resets_.push_back(i);
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
                double dt, double rate) {
  const size_t n = std::min({progress.size(), cross.size(), speed.size()});
  constexpr float kWidth = 2.0f;  // a car's width, m (ours)
  for (size_t i = 0; i < ai && i < n && i < lane.size(); ++i) {
    const LaneParams& p = params[std::min(i, params.size() - 1)];
    // The nearest car ahead within the trailing distance, in the way and slower.
    size_t block = n;
    double best = 1e30;
    for (size_t j = 0; j < n; ++j) {
      if (j == i) continue;
      const double gap = progress[j] - progress[i];
      if (gap <= 0 || gap > p.trailing || gap >= best) continue;
      if (std::fabs(cross[j] - cross[i]) > kWidth + p.car_clearance) continue;
      if (speed[i] <= speed[j]) continue;
      block = j, best = gap;
    }
    float target = 0;
    const double right = line.room(progress[i], true), left = line.room(progress[i], false);
    if (block < n) {
      const double hw = right < 1e8 ? 0.5 * (right + left) : 4.0;
      const float shift = float(0.5 * hw + p.car_clearance);
      // Keep a side once chosen; else the side with more room past the car ahead.
      float side = lane[i] > 0.3f ? 1.0f : lane[i] < -0.3f ? -1.0f : 0.0f;
      if (side == 0) side = (right - cross[block]) >= (left + cross[block]) ? 1.0f : -1.0f;
      target = side * shift;
    }
    if (right < 1e8)
      target = std::clamp(target, -float(std::max(0.0, left - p.edge_clearance)),
                          float(std::max(0.0, right - p.edge_clearance)));
    const float step = float(rate * dt);
    lane[i] += std::clamp(target - lane[i], -step, step);
  }
}

}  // namespace race
