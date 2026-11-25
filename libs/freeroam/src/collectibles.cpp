#include "freeroam/collectibles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include "audio/radio_dj.h"
#include "audio/xml.h"
#include "fh1data/archive.h"

namespace freeroam {

namespace {

bool fail(std::string* error, const std::string& what) {
  if (error) *error = what;
  return false;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::array<double, 3> axis(const audio::XmlNode* n, std::array<double, 3> fallback) {
  if (!n) return fallback;
  return {n->number("x"), n->number("y"), n->number("z")};
}

}  // namespace

bool parse_collectibles(std::string_view flyers_xml, std::string_view barns_xml, std::string_view game_objs_xml,
                        Collectibles& out, std::string* error) {
  out = Collectibles{};
  audio::XmlNode flyers, barns, objs;
  std::string why;
  if (!audio::parse_xml(flyers_xml, flyers, &why)) return fail(error, "Flyers.xml: " + why);
  if (!audio::parse_xml(barns_xml, barns, &why)) return fail(error, "barnfinds.xml: " + why);
  if (!audio::parse_xml(game_objs_xml, objs, &why)) return fail(error, "GameObjs.xml: " + why);

  std::map<std::string, Placement> placed;  // lower-case GameplayID
  for (const auto& o : objs.children) {
    if (!o.has("GameplayID")) continue;
    Placement p;
    if (const auto* pos = o.child("Pos")) p.x = pos->number("x"), p.y = pos->number("y"), p.z = pos->number("z");
    if (const auto* r = o.child("Orientation")) {
      p.x_axis = axis(r->child("XAxis"), p.x_axis);
      p.y_axis = axis(r->child("YAxis"), p.y_axis);
      p.z_axis = axis(r->child("ZAxis"), p.z_axis);
    }
    placed.emplace(lower(o.attr("GameplayID")), p);
  }

  for (const auto& a : flyers.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityFlyers")) continue;
    Sign s;
    s.id = lower(a.attr("name"));
    auto it = placed.find(s.id);
    if (it == placed.end()) return fail(error, "sign " + s.id + " is not placed in GameObjs.xml");
    s.at = it->second;
    out.signs.push_back(std::move(s));
  }

  for (const auto& a : barns.children) {
    if (!audio::iequals(a.name, "Activity") || !audio::iequals(a.attr("type"), "ActivityBarnFind")) continue;
    Barn b;
    b.name = a.attr("name");
    const auto* trigger = a.child("TriggerZone");
    const auto* hint = a.child("HintRegion");
    const auto* doors = a.child("DoorInfo");
    const auto* car = a.child("UnlockCar");
    if (!trigger || !hint || !doors || !car) return fail(error, "barn find " + b.name + " lacks an element");
    b.trigger = lower(trigger->attr("object"));
    auto it = placed.find(b.trigger);
    if (it == placed.end()) return fail(error, "barn find object " + b.trigger + " is not placed in GameObjs.xml");
    b.at = it->second;
    b.radius = trigger->number("radius", 10);
    b.max_mph = trigger->number("maxMPH", 50);
    // x_offset east (+X), y_offset north (+Z): the spec's probable sign.
    b.hint_x = b.at.x + hint->number("x_offset");
    b.hint_z = b.at.z + hint->number("y_offset");
    b.hint_rx = hint->number("x_radius");
    b.hint_rz = hint->number("y_radius", b.hint_rx);
    b.open_id = lower(doors->attr("open_id"));
    b.closed_id = lower(doors->attr("closed_id"));
    b.open_coll_id = lower(doors->attr("open_coll_id"));
    b.closed_coll_id = lower(doors->attr("closed_coll_id"));
    b.car_id = int(car->number("id"));
    b.car_name_key = car->attr("stringTableNameId");
    // SpawnInfo and DistanceDriven distances are miles in the file (the
    // title's loader scales them by 1609.344); HintRegion is metres.
    constexpr double kMile = 1609.344;
    if (const auto* sp = a.child("SpawnInfo")) {
      b.spawn_weighting = int(sp->number("weighting", 100));
      b.spawn_min_probability = sp->number("min_probability", 1);
      b.spawn_max_probability = sp->number("max_probability", 1);
      b.spawn_min_distance_m = sp->number("min_distance") * kMile;
      b.spawn_max_distance_m = sp->number("max_distance") * kMile;
    }
    if (const auto* dd = a.child("DistanceDriven")) {
      b.restore_min_m = dd->number("distanceMin") * kMile;
      b.restore_max_m = dd->number("distanceMax") * kMile;
      b.restore_threshold_m = dd->number("threshold") * kMile;
    }
    if (const auto* dj = a.child("RadioDJRequests")) {
      for (const auto& r : audio::parse_radio_dj_requests(*dj))
        b.radio_requests.push_back({r.channel == audio::RadioChannel::kImmediate, r.event, r.delay_min_s, r.delay_max_s,
                                    r.enabled, r.top_priority});
    }
    out.barns.push_back(std::move(b));
  }
  if (out.signs.empty()) return fail(error, "Flyers.xml lists no signs");
  return true;
}

bool load_collectibles(const std::filesystem::path& game_root, Collectibles& out, std::string* error) {
  const auto media = game_root / "media";
  std::string why;
  auto modes = fh1::Archive::open(media / "gamemodes.zip", &why);
  if (!modes) return fail(error, why);
  auto flyers = modes->read("Colorado/Flyers.xml");
  if (!flyers.ok()) return fail(error, "gamemodes.zip Colorado/Flyers.xml: " + flyers.error);
  auto barns = modes->read("Colorado/barnfinds.xml");
  if (!barns.ok()) return fail(error, "gamemodes.zip Colorado/barnfinds.xml: " + barns.error);
  const auto objs_path = media / "tracks" / "colorado" / "Ribbon_00" / "GameObjs.xml";
  std::ifstream f(objs_path, std::ios::binary);
  if (!f) return fail(error, "cannot read " + objs_path.string());
  std::stringstream objs;
  objs << f.rdbuf();
  auto text = [](const std::vector<uint8_t>& d) { return std::string_view(reinterpret_cast<const char*>(d.data()), d.size()); };
  return parse_collectibles(text(flyers.data), text(barns.data), objs.str(), out, error);
}

double barn_restore_distance(const Barn& b, double found_at) {
  const double t = b.restore_threshold_m > 0 ? std::clamp(1.0 - found_at / b.restore_threshold_m, 0.0, 1.0) : 0.0;
  return b.restore_min_m + (b.restore_max_m - b.restore_min_m) * t;
}

FreeRoam::FreeRoam(const Collectibles& data, Progress& progress) : data_(data), progress_(progress) {
  progress_.barns.resize(data_.barns.size());
  sign_found_.assign(data_.signs.size(), 0);
  for (size_t i = 0; i < data_.signs.size(); ++i) sign_found_[i] = progress_.signs_found.count(data_.signs[i].id) > 0;
  rebuild_hidden();
}

void FreeRoam::set_barn_tuning(double time_between_ticks_s, double max_idle_time_s, double min_distance_before_next_m,
                               double min_speed_before_spawn_ms, double min_time_before_spawn_s) {
  tick_s_ = time_between_ticks_s, max_idle_s_ = max_idle_time_s, min_gap_m_ = min_distance_before_next_m;
  min_speed_ = min_speed_before_spawn_ms, min_time_s_ = min_time_before_spawn_s;
}

double FreeRoam::uniform() {
  if (uniform_) return std::clamp(uniform_(), 0.0, 1.0);
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

int FreeRoam::barns_found() const {
  int n = 0;
  for (const auto& b : progress_.barns) n += barn_discovered(b.status);
  return n;
}

bool collect_barn(const Collectibles& data, Progress& progress, size_t index) {
  if (index >= progress.barns.size() || index >= data.barns.size()) return false;
  BarnProgress& bp = progress.barns[index];
  if (bp.status != kBarnReady) return false;
  bp.status = kBarnCollected;
  const int car = data.barns[index].car_id;
  if (car > 0 && std::find(progress.garage.begin(), progress.garage.end(), car) == progress.garage.end())
    progress.garage.push_back(car);
  return true;
}

bool FreeRoam::collect_barn(size_t index) {
  if (!freeroam::collect_barn(data_, progress_, index)) return false;
  rebuild_hidden();
  return true;
}

void FreeRoam::rebuild_hidden() {
  auto set = std::make_shared<std::set<std::string>>(progress_.signs_found);
  for (size_t i = 0; i < data_.barns.size(); ++i) {
    const Barn& b = data_.barns[i];
    if (barn_discovered(progress_.barns[i].status)) {
      set->insert({b.closed_id, b.closed_coll_id, b.trigger});
    } else {
      set->insert({b.open_id, b.open_coll_id});
    }
  }
  set->erase(std::string());
  hidden_ = std::move(set);
}

std::vector<Event> FreeRoam::step(const CarState& car, const BarnContext& ctx) {
  std::vector<Event> events;
  bool changed = false;
  progress_.total_distance += std::max(0.0, car.moved);

  // Signs: 2D XZ OBB-vs-OBB overlap between the car's swept footprint and each
  // sign's oriented billboard bounds (interim break rule: contact above 1 m/s).
  if (std::fabs(car.speed) > kSignBreakSpeed) {
    const double fl = std::hypot(car.forward_x, car.forward_z);
    const double fx = fl > 1e-6 ? car.forward_x / fl : 0.0;
    const double fz = fl > 1e-6 ? car.forward_z / fl : 1.0;
    // Right axis in game frame (+Y up, +Z forward): right = (fz, -fx).
    const double rx = fz, rz = -fx;
    // Extend the car footprint backward along its travel direction by `car.moved`
    // so fast vehicles cannot tunnel through thin billboard signs between steps.
    const double sweep = std::clamp(car.moved, 0.0, 6.0);
    const double travel_sign = car.speed >= 0.0 ? 1.0 : -1.0;
    const double car_cx = car.x - fx * (0.5 * sweep * travel_sign);
    const double car_cz = car.z - fz * (0.5 * sweep * travel_sign);
    const double car_hl = std::max(0.5, car.half_length) + 0.5 * sweep;
    const double car_hw = std::max(0.4, car.half_width);
    const double max_r = car_hl + car_hw + kSignBoardHalfWidth + 1.0;
    const double max_r2 = max_r * max_r;

    for (size_t i = 0; i < data_.signs.size(); ++i) {
      if (sign_found_[i]) continue;
      const Placement& p = data_.signs[i].at;
      const double dx = p.x - car_cx, dz = p.z - car_cz;
      if (dx * dx + dz * dz > max_r2 || std::fabs(p.y - car.y) > kSignHeightTolerance) continue;

      // Sign horizontal board axes (local X = board width, local Z = board thickness).
      double sx_x = p.x_axis[0], sx_z = p.x_axis[2];
      const double sxl = std::hypot(sx_x, sx_z);
      if (sxl > 1e-4) {
        sx_x /= sxl;
        sx_z /= sxl;
      } else {
        sx_x = 1.0;
        sx_z = 0.0;
      }
      double sz_x = p.z_axis[0], sz_z = p.z_axis[2];
      const double dot_xz = sz_x * sx_x + sz_z * sx_z;
      sz_x -= dot_xz * sx_x;
      sz_z -= dot_xz * sx_z;
      const double szl = std::hypot(sz_x, sz_z);
      if (szl > 1e-4) {
        sz_x /= szl;
        sz_z /= szl;
      } else {
        sz_x = -sx_z;
        sz_z = sx_x;
      }

      auto separated = [&](double ax, double az) {
        const double dist = std::fabs(dx * ax + dz * az);
        const double r_car = car_hl * std::fabs(fx * ax + fz * az) + car_hw * std::fabs(rx * ax + rz * az);
        const double r_sign =
            kSignBoardHalfWidth * std::fabs(sx_x * ax + sx_z * az) +
            kSignBoardHalfDepth * std::fabs(sz_x * ax + sz_z * az);
        return dist > r_car + r_sign;
      };
      if (separated(fx, fz) || separated(rx, rz) || separated(sx_x, sx_z) || separated(sz_x, sz_z)) continue;

      sign_found_[i] = 1;
      progress_.signs_found.insert(data_.signs[i].id);
      events.push_back({Event::kSignSmashed, int(i)});
      changed = true;
    }
  }

  step_barns(car, ctx, events, changed);

  if (changed) rebuild_hidden();
  return events;
}

void FreeRoam::step_barns(const CarState& car, const BarnContext& ctx, std::vector<Event>& events, bool& changed) {
  const double odo = progress_.total_distance;
  const double speed = std::fabs(car.speed);
  const double dt = std::max(0.0, car.dt);
  const size_t n = std::min(data_.barns.size(), progress_.barns.size());
  const bool live = ctx.free_roam && !ctx.blocked;

  // Discovery: a rumoured barn, inside its trigger zone below maxMPH, in
  // free roam. Restoration starts (status 1) at this odometer.
  for (size_t i = 0; i < n && live; ++i) {
    BarnProgress& bp = progress_.barns[i];
    if (bp.status != kBarnRumour) continue;
    const Barn& b = data_.barns[i];
    if (std::hypot(b.at.x - car.x, b.at.z - car.z) > b.radius) continue;
    if (speed >= b.max_mph * 0.44704) continue;
    bp.status = kBarnRestoring;
    bp.found_at = std::trunc(odo);
    events.push_back({Event::kBarnFound, int(i)});
    changed = true;
  }

  // Restoration: ready once the restoration distance is driven, in free
  // roam above 25 mph (the speed check is probable, SPEC Q116).
  constexpr double kReadySpeed = 11.176;
  for (size_t i = 0; i < n && ctx.free_roam; ++i) {
    BarnProgress& bp = progress_.barns[i];
    if (bp.status != kBarnRestoring) continue;
    if (odo - bp.found_at < barn_restore_distance(data_.barns[i], bp.found_at)) continue;
    if (speed < kReadySpeed) continue;
    bp.status = kBarnReady;
    events.push_back({Event::kBarnReady, int(i)});
    changed = true;
  }

  // The spawn tick: one roll every TimeBetweenTicks, none while idle for
  // MaxIdleTime. A pending barn waits for its rumour; otherwise a weighted
  // pick among the unspawned barns rolls its 1-in-N odds at this odometer.
  idle_s_ = speed < 0.5 ? idle_s_ + dt : 0.0;
  int pending = -1;
  for (size_t i = 0; i < n; ++i)
    if (progress_.barns[i].status == kBarnPending) pending = int(i);
  tick_clock_ += dt;
  if (tick_s_ > 0 && tick_clock_ >= tick_s_) {
    tick_clock_ = 0;
    if (idle_s_ < max_idle_s_ && pending < 0) {
      double total = 0;
      for (size_t i = 0; i < n; ++i)
        if (progress_.barns[i].status == kBarnNotSpawned) total += std::max(0, data_.barns[i].spawn_weighting);
      int pick = -1;
      if (total > 0) {
        double r = uniform() * total;
        for (size_t i = 0; i < n; ++i) {
          if (progress_.barns[i].status != kBarnNotSpawned) continue;
          const double w = std::max(0, data_.barns[i].spawn_weighting);
          if (w <= 0) continue;
          pick = int(i);
          if (r < w) break;
          r -= w;
        }
      }
      if (pick >= 0) {
        const Barn& b = data_.barns[size_t(pick)];
        const double span = b.spawn_max_distance_m - b.spawn_min_distance_m;
        const double t = span != 0 ? (odo - b.spawn_min_distance_m) / span : 1.0;
        const double lo = std::min(b.spawn_min_probability, b.spawn_max_probability);
        const double hi = std::max(b.spawn_min_probability, b.spawn_max_probability);
        const double p =
            std::clamp(b.spawn_min_probability + t * (b.spawn_max_probability - b.spawn_min_probability), lo, hi);
        const int odds = std::max(1, int(std::lround(p)));
        const bool hit = odds <= 1 || std::lround(uniform() * double(odds - 1)) == 0;
        if (hit) {
          progress_.barns[size_t(pick)].status = kBarnPending;
          pending = pick;
          pending_s_ = 0;
        }
      }
    }
  }

  // Pending -> rumour: the timer runs in free roam (reset outside it) while
  // the car is above MinSpeedBeforeSpawn, no dialogue plays and the odometer
  // is MinDistanceBeforeNextSpawn past the latest rumour start (paused
  // otherwise); after MinTimeBeforeSpawn the rumour starts.
  if (pending >= 0) {
    if (!live) {
      pending_s_ = 0;
    } else {
      double latest = -1;
      for (size_t i = 0; i < n; ++i) {
        const int st = progress_.barns[i].status;
        if (st == kBarnRumour || barn_discovered(st)) latest = std::max(latest, progress_.barns[i].rumour_at);
      }
      const bool far_enough = latest < 0 || odo - latest > min_gap_m_;
      if (far_enough && speed > min_speed_ && !ctx.dialogue) pending_s_ += dt;
      if (pending_s_ >= min_time_s_) {
        BarnProgress& bp = progress_.barns[size_t(pending)];
        bp.status = kBarnRumour;
        bp.rumour_at = std::trunc(odo);
        pending_s_ = 0;
        events.push_back({Event::kRumourStarted, pending});
      }
    }
  }
}

int64_t discounted_price(int64_t price, int discount_percent) {
  const int d = upgrade_discount(discount_percent);
  if (d <= 0) return price;
  const float f = (1.0f - float(d) / 100.0f) * float(price) + 0.5f;
  return int64_t(std::floor(f));
}

std::array<double, 2> map_uv(double x, double z) {
  return {(x - kMapWest) / kMapWidth, (kMapNorth - z) / kMapHeight};
}

}  // namespace freeroam
