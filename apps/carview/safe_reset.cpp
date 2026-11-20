#include "safe_reset.h"

#include <cmath>

#include "traffic/nav.h"
#include "traffic/roads.h"
#include "world/collision.h"
#include "world/road_follow.h"

namespace carview {

SafeReset::SafeReset() = default;
SafeReset::~SafeReset() = default;

bool SafeReset::open(const std::filesystem::path& game_root, std::string* error) {
  traffic::NavMap map;
  if (!traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", map, error)) return false;
  roads_ = std::make_unique<traffic::Roads>();
  roads_->build(map);
  return true;
}

bool SafeReset::place(const world::Streamer& streamer, drive::Vehicle& vehicle, double* distance) const {
  if (!roads_) return false;
  constexpr double kReach = 60, kStep = 4, kClearance = 3;
  const drive::Vec3 at = vehicle.position();
  const traffic::RoadHit hit = roads_->nearest(at.x, at.z, false, kReach);
  if (hit.road < 0) return false;
  const drive::Vec3 fwd = drive::rotate(vehicle.orientation(), {0, 0, 1});
  const auto& ground = streamer.ground();
  // Nearest first: 0, +4, -4, +8, -8 ... m along the road.
  for (int k = 0; k <= 2 * int(kReach / kStep); ++k) {
    const double ds = (k % 2 ? 1 : -1) * double((k + 1) / 2) * kStep;
    std::array<double, 2> dir{};
    const traffic::P3 c = roads_->point_at(hit.road, hit.s + ds, nullptr, &dir);
    double y = 0;
    drive::Hit h;
    if (!world::ground_height(ground, c.x, c.z, c.y + 2, y, &h)) continue;
    if (std::fabs(y - c.y) > 2 || h.normal.y < 0.85) continue;
    const drive::Vec3 p{c.x, y, c.z};
    bool clear = true;
    for (int a = 0; a < 8 && clear; ++a) {
      const double t = a * 3.14159265358979323846 / 4;
      if (world::wall_between(ground, p, c.x + kClearance * std::sin(t), c.z + kClearance * std::cos(t))) clear = false;
    }
    if (!clear) continue;
    double hx = dir[0], hz = dir[1];
    if (!roads_->roads()[size_t(hit.road)].oneway && hx * fwd.x + hz * fwd.z < 0) hx = -hx, hz = -hz;
    vehicle.place({c.x, y + 0.05, c.z}, std::atan2(hx, hz));
    if (distance) *distance = std::hypot(c.x - at.x, c.z - at.z);
    return true;
  }
  return false;
}

}  // namespace carview
