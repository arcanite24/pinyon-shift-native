#include "traffic_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

#include "drive/cameras.h"
#include "drive/from_bake.h"
#include "drive/from_physics.h"

namespace carview {
namespace {

// Column-major 4x4 helpers in double (the world is kilometres across).
void to_double(const float* m, double* out) {
  for (int i = 0; i < 16; ++i) out[i] = m[i];
}

void invert_affine(const double* m, double* out) {
  const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
  const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  const double id = det != 0 ? 1.0 / det : 0.0;
  const double inv[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                         (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                         (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};  // row-major
  for (int i = 0; i < 16; ++i) out[i] = 0;
  for (int r = 0; r < 3; ++r)
    for (int col = 0; col < 3; ++col) out[col * 4 + r] = inv[r * 3 + col];
  for (int r = 0; r < 3; ++r) out[12 + r] = -(inv[r * 3 + 0] * m[12] + inv[r * 3 + 1] * m[13] + inv[r * 3 + 2] * m[14]);
  out[15] = 1;
}

void multiply(const double* a, const double* b, float* out) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = float(sum);
    }
}

// The orientation of a car on its lane: yaw, then its nose up by pitch.
drive::Quat lane_orientation(double heading, double pitch) {
  const drive::Quat y{0, std::sin(heading / 2), 0, std::cos(heading / 2)};
  const drive::Quat t{std::sin(-pitch / 2), 0, 0, std::cos(-pitch / 2)};
  return {y.w * t.x + y.x * t.w + y.y * t.z - y.z * t.y, y.w * t.y - y.x * t.z + y.y * t.w + y.z * t.x,
          y.w * t.z + y.x * t.y - y.y * t.x + y.z * t.w, y.w * t.w - y.x * t.x - y.y * t.y - y.z * t.z};
}

// Per-car overhead roof probe (35 m upward BVH raycast) so each traffic car toggles
// tunnel headlamps based on its own roof occlusion rather than the player's state.
bool is_under_tunnel_roof(const drive::Ground* ground, const drive::Vec3& pos) {
  if (!ground) return false;
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
  drive::Hit hit{};
  if (!ground->raycast({pos.x, pos.y + 1.6, pos.z}, {0.0, 1.0, 0.0}, 35.0, hit)) return false;
  return hit.distance >= 0.5 && std::fabs(hit.normal.y) > 0.40;
}

}  // namespace

TrafficView::TrafficView(Renderer& renderer, Showroom& host, std::filesystem::path bake_dir, bool sync_loads)
    : r_(renderer), host_(host), bake_dir_(std::move(bake_dir)), sync_(sync_loads) {}

TrafficView::~TrafficView() {
  drive::clear_camera_vehicle_obbs(drive::CameraObstacleSource::kTraffic);
  drive::clear_camera_vehicle_obbs(drive::CameraObstacleSource::kRaceAi);
  host_.set_instances({});
  for (auto& [id, g] : guests_)
    if (g.loading.valid()) g.loading.wait();
  r_.wait_idle();  // one single device idle on shutdown instead of one per guest
  for (auto& [id, g] : guests_)
    for (auto& sr : g.showrooms)
      if (sr) sr->skip_wait_idle_on_destroy();
  guests_.clear();
}

bool TrafficView::open(const std::filesystem::path& game_root, const drive::Ground* const* ground, double hour,
                       uint64_t seed, std::string& error, bool race_mode) {
  game_root_ = game_root;
  if (!traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", map_, &error) ||
      !traffic::read_traffic_config(game_root, config_, &error, race_mode))
    return false;
  roads_.build(map_);
  // The traffic models, by Data_Car.Id: from bake_dir when present, else
  // directly from the game disc / DLC packages via Showroom's disc cache.
  std::error_code ec;
  if (!bake_dir_.empty()) {
    for (const auto& f : std::filesystem::directory_iterator(bake_dir_ / "cars", ec)) {
      const std::string name = f.path().filename().string();
      if (name.size() < 12 || name.substr(name.size() - 12) != ".physics.txt") continue;
      std::ifstream in(f.path());
      std::string line;
      for (int k = 0; k < 4 && std::getline(in, line) && line.rfind("car ", 0) != 0; ++k) {
      }
      long long id = 0;
      char media[128] = {};
      if (std::sscanf(line.c_str(), "car %lld %127s", &id, media) != 2) continue;
      for (const auto& m : config_.models)
        if (m.id == id && std::filesystem::exists(bake_dir_ / "cars" / (name.substr(0, name.size() - 12) + ".shiftbake")))
          media_[id] = media;
    }
  }
  if (media_.empty() && !game_root.empty()) {
    const char* sr = std::getenv("SHIFT_STATE_ROOT");
    std::map<int64_t, std::string> disc_ids;
    std::vector<std::string> disc_media;
    std::string disc_err;
    if (Showroom::discover_disc_cars(game_root, sr ? std::filesystem::path(sr) : std::filesystem::path{},
                                     disc_ids, disc_media, &disc_err)) {
      for (const auto& m : config_.models) {
        if (auto it = disc_ids.find(m.id); it != disc_ids.end()) media_[m.id] = it->second;
      }
    }
  }
  if (media_.empty()) {
    error = "traffic: none of the freeroam traffic models is available" +
            (bake_dir_.empty() ? std::string(" from disc") : (" in " + bake_dir_.string()));
    return false;
  }
  population_ = std::make_unique<traffic::Population>(roads_, config_, seed);
  std::vector<int64_t> available;
  for (const auto& [id, m] : media_) available.push_back(id);
  population_->set_available(available);
  population_->set_hour(hour);
  ground_ = ground;
  population_->set_ground([this](double x, double z, double hint, double& y) {
    if (!ground_ || !*ground_) return false;
    drive::Hit h;
    if (!(*ground_)->raycast({x, hint + 4, z}, {0, -1, 0}, 12, h)) return false;
    y = hint + 4 - h.distance;
    return true;
  });
  const traffic::BodiesOptions bo = traffic::bodies_options(config_);
  bodies_ = std::make_unique<traffic::Bodies>(
      *population_,
      [this](int64_t m) -> const drive::CarParams* {
        const auto it = params_.find(m);
        return it == params_.end() ? nullptr : &it->second;
      },
      bo);
  std::printf("traffic: %zu roads, %zu of %zu models baked, %02d:%02d\n", roads_.roads().size(), media_.size(),
              config_.models.size(), int(hour), int(std::fmod(hour, 1.0) * 60));
  return true;
}

void TrafficView::set_external_vehicles(std::vector<traffic::ExternalVehicle> vehicles) {
  external_vehicles_ = std::move(vehicles);
  external_ttl_ = external_vehicles_.empty() ? 0 : 2;
  std::vector<drive::CameraVehicleObb> race_obbs;
  race_obbs.reserve(external_vehicles_.size());
  for (const auto& ev : external_vehicles_) {
    const double spd = std::hypot(ev.velocity.x, ev.velocity.z);
    const double heading = spd > 0.25 ? std::atan2(ev.velocity.x, ev.velocity.z) : 0.0;
    const double ch = std::cos(heading), sh = std::sin(heading);
    drive::CameraVehicleObb obb;
    obb.axis_x = {ch, 0.0, -sh};
    obb.axis_y = {0.0, 1.0, 0.0};
    obb.axis_z = {sh, 0.0, ch};
    obb.half_extents = {0.98, 0.72, 2.25};
    obb.center = {ev.position.x, ev.position.y + 0.72, ev.position.z};
    race_obbs.push_back(obb);
  }
  drive::set_camera_vehicle_obbs(drive::CameraObstacleSource::kRaceAi, std::move(race_obbs));
}

void TrafficView::publish_camera_obbs(const drive::Vec3& player_pos) const {
  if (!population_) return;
  std::vector<drive::CameraVehicleObb> obbs;
  for (const auto& c : population_->cars()) {
    const drive::Vehicle* body = bodies_ ? bodies_->body(c.id) : nullptr;
    const drive::Vec3 at = body ? body->position() : drive::Vec3{c.position.x, c.position.y, c.position.z};
    const double dx = at.x - player_pos.x, dy = at.y - player_pos.y, dz = at.z - player_pos.z;
    if (dx * dx + dy * dy + dz * dz > 25.0 * 25.0) continue;
    const drive::Quat rot = body ? body->orientation() : lane_orientation(c.heading, c.pitch);
    const drive::Vec3 ax = drive::rotate(rot, {1.0, 0.0, 0.0});
    const drive::Vec3 ay = drive::rotate(rot, {0.0, 1.0, 0.0});
    const drive::Vec3 az = drive::rotate(rot, {0.0, 0.0, 1.0});
    drive::Vec3 sz{1.9, 1.5, 4.4};
    if (body) {
      sz = body->params().size;
    } else {
      const auto lit = locked_model_.find(c.id);
      const int64_t mid = (lit != locked_model_.end()) ? lit->second : c.model;
      if (const auto pit = params_.find(mid); pit != params_.end()) sz = pit->second.size;
    }
    const double hx = std::clamp(0.5 * sz.x, 0.75, 1.65);
    const double hy = std::clamp(0.5 * sz.y, 0.55, 2.20);
    const double hz = std::clamp(0.5 * sz.z, 1.60, 6.50);
    drive::CameraVehicleObb obb;
    obb.axis_x = {ax.x, ax.y, ax.z};
    obb.axis_y = {ay.x, ay.y, ay.z};
    obb.axis_z = {az.x, az.y, az.z};
    obb.half_extents = {hx, hy, hz};
    obb.center = {at.x + ay.x * hy, at.y + ay.y * hy, at.z + ay.z * hy};
    obbs.push_back(obb);
  }
  drive::set_camera_vehicle_obbs(drive::CameraObstacleSource::kTraffic, std::move(obbs));
}

void TrafficView::step(drive::Vehicle& player, std::span<const traffic::ExternalVehicle> obstacles) {
  if (!population_) return;
  std::map<uint32_t, TrafficPose> snap;
  for (const auto& c : population_->cars()) {
    const drive::Vehicle* body = bodies_ ? bodies_->body(c.id) : nullptr;
    snap[c.id] = {
        body ? body->position() : drive::Vec3{c.position.x, c.position.y, c.position.z},
        body ? body->orientation() : lane_orientation(c.heading, c.pitch),
    };
  }
  before_ = std::move(snap);
  const drive::Vec3 p = player.position(), v = player.velocity();
  const drive::Vec3 fwd = drive::rotate(player.orientation(), {0, 0, 1});
  const double heading = std::atan2(fwd.x, fwd.z);
  const std::span<const traffic::ExternalVehicle> active_obs =
      obstacles.empty() ? std::span<const traffic::ExternalVehicle>(external_vehicles_) : obstacles;
  population_->step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z}, heading, active_obs);
  if (external_ttl_ > 0 && --external_ttl_ == 0) {
    external_vehicles_.clear();
    drive::clear_camera_vehicle_obbs(drive::CameraObstacleSource::kRaceAi);
  }
  // The bodies' parameters come with the models: refresh them on the sim's
  // clock too, so a run that renders seldom (headless --every) still gets
  // bodies near the player.
  if (population_->time() - guests_at_ >= 0.25) {
    guests_at_ = population_->time();
    update_guests();
  }
  if (ground_ && *ground_) bodies_->step(**ground_, &player);
  publish_camera_obbs(p);
}

void TrafficView::update_guests() {
  // The models wanted: the loaded set, any with a car still driving, and any
  // model permanently locked to a visible car so locked cars never lose their mesh.
  std::set<int64_t> want(population_->loaded().begin(), population_->loaded().end());
  for (const auto& c : population_->cars()) want.insert(c.model);
  for (const auto& [cid, mid] : locked_model_) want.insert(mid);
  for (int64_t id : want) {
    if (!media_.count(id)) continue;
    Guest& g = guests_[id];
    const auto dit = desired_slots_.find(id);
    const size_t desired =
        std::clamp(dit != desired_slots_.end() ? dit->second : size_t(1), size_t(1), kMaxWheelClonesPerModel);
    while (g.showrooms.size() < desired && !g.failed) {
      if (!g.loading.valid()) {
        const std::filesystem::path bake = bake_dir_;
        const std::filesystem::path root = game_root_;
        const std::string media = media_[id];
        Renderer* r = &r_;
        const VkImageView host_env_view = host_.dynamic_env_view();
        const VkSampler host_env_sampler = host_.dynamic_env_sampler();
        const bool upload_on_worker = !sync_;
        g.loading = std::async(std::launch::async, [r, bake, root, media, upload_on_worker, host_env_view, host_env_sampler]() {
          Loaded out;
          auto s = std::make_unique<Showroom>(*r);
          s->set_guest();
          if (host_env_view && host_env_sampler) s->set_shared_dynamic_env(host_env_view, host_env_sampler);
          std::string e;
          ShowroomOptions opt{};
          opt.game_root = root;
          if (!s->load(bake, media, opt, e)) {
            std::fprintf(stderr, "carview: traffic model %s: %s\n", media.c_str(), e.c_str());
            return out;
          }
          s->load_car_lamps(root, media);
          if (upload_on_worker) {
            const auto t0 = std::chrono::steady_clock::now();
            if (!s->upload(e)) {
              out.error = e;
              out.showroom = std::move(s);
              return out;
            }
            out.upload_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            out.uploaded = true;
          }
          out.showroom = std::move(s);
          return out;
        });
      }
      if (!sync_ && g.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;
      Loaded loaded = g.loading.get();
      auto s = std::move(loaded.showroom);
      if (!s) {
        g.failed = true;
        break;
      }
      if (!loaded.error.empty()) {
        std::fprintf(stderr, "carview: traffic model %s: %s\n", media_[id].c_str(), loaded.error.c_str());
        g.failed = true;
        break;
      }
      double ms = loaded.upload_ms;
      if (!loaded.uploaded) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string e;
        if (!s->upload(e)) {
          std::fprintf(stderr, "carview: traffic model %s: %s\n", media_[id].c_str(), e.c_str());
          g.failed = true;
          break;
        }
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }
      upload_ms_max_ = std::max(upload_ms_max_, loaded.uploaded ? 0.0 : ms);  // render-thread stall only
      ++uploads_;
      const bool is_primary = g.showrooms.empty();
      if (is_primary) {
        params_[id] = drive::params_for_baked_car(s->car(), bake_dir_);
        std::printf("traffic: t %.1f s: model %lld (%s) uploaded in %.0f ms\n", population_->time(), (long long)id,
                    media_[id].c_str(), ms);
      }
      g.showrooms.push_back(std::move(s));
      if (!sync_) break;  // async mode: load at most one extra clone at a time per model
    }
  }
  // Models neither loaded nor driving: dropped (out of the instances first).
  for (auto it = guests_.begin(); it != guests_.end();) {
    if (want.count(it->first) || it->second.loading.valid()) {
      ++it;
      continue;
    }
    host_.set_instances({});
    params_.erase(it->first);
    desired_slots_.erase(it->first);
    std::printf("traffic: t %.1f s: model %lld (%s) dropped\n", population_->time(), (long long)it->first,
                media_[it->first].c_str());
    // Retire dropped traffic GPU resources through the host's multi-frame retirement ring
    // instead of calling vkDeviceWaitIdle() on the render thread (eliminates 4.5-13.1 ms hitches).
    for (auto& sr : it->second.showrooms) {
      if (sr) {
        std::shared_ptr<Showroom> retired = std::move(sr);
        retired->skip_wait_idle_on_destroy();
        host_.retire_deferred([g = std::move(retired)]() mutable { g.reset(); });
      }
    }
    it = guests_.erase(it);
  }
}

void TrafficView::place(const cars::bake::CarInfo& player_info, const drive::Vehicle& player) {
  if (!population_) return;
  const drive::Vec3 player_pos = player.position();
  std::set<uint32_t> alive_ids;
  for (const auto& c : population_->cars()) alive_ids.insert(c.id);
  for (auto it = locked_model_.begin(); it != locked_model_.end();)
    it = alive_ids.count(it->first) ? std::next(it) : locked_model_.erase(it);
  // Count nearby visible cars per effective model (< 95 m) so identical nearby traffic cars each receive
  // an independent Showroom clone with isolated wheel steer, spin, omega blur, and suspension Y.
  {
    std::map<int64_t, size_t> near_counts;
    for (const auto& c : population_->cars()) {
      if (std::clamp(c.alpha, 0.0f, 1.0f) <= 0.005f) continue;
      const drive::Vehicle* body = bodies_ ? bodies_->body(c.id) : nullptr;
      const double cx = body ? body->position().x : c.position.x;
      const double cy = body ? body->position().y : c.position.y;
      const double cz = body ? body->position().z : c.position.z;
      const double dx = cx - player_pos.x, dy = cy - player_pos.y, dz = cz - player_pos.z;
      if (dx * dx + dy * dy + dz * dz <= 95.0 * 95.0) {
        const auto lit = locked_model_.find(c.id);
        const int64_t eff_model = (lit != locked_model_.end()) ? lit->second : c.model;
        size_t& cnt = near_counts[eff_model];
        if (cnt < kMaxWheelClonesPerModel) ++cnt;
      }
    }
    for (const auto& [id, cnt] : near_counts) {
      desired_slots_[id] = std::max(desired_slots_[id], cnt);
    }
  }
  update_guests();
  float pm[16];
  drive::model_to_world(player_info, player, pm);
  double pmd[16], world_to_player[16];
  to_double(pm, pmd);
  invert_affine(pmd, world_to_player);
  const double now_t = population_->time();
  const float lamp_dt = lamps_at_ >= 0 ? float(std::clamp(now_t - lamps_at_, 0.0, 0.25)) : float(drive::Vehicle::kStep);
  lamps_at_ = now_t;
  for (auto it = lamps_.begin(); it != lamps_.end();)
    it = alive_ids.count(it->first) ? std::next(it) : lamps_.erase(it);
  for (auto it = wheel_spin_.begin(); it != wheel_spin_.end();)
    it = alive_ids.count(it->first) ? std::next(it) : wheel_spin_.erase(it);

  // Collect all currently resident traffic models (sorted deterministically by model ID).
  // Once a spawned car is first rendered with any resident model, locked_model_[car_id]
  // pins it permanently to that model for its entire visible lifetime so async uploads
  // never visibly morph a car into a different vehicle mid-road.
  std::vector<int64_t> resident_models;
  resident_models.reserve(guests_.size());
  for (const auto& [gid, gg] : guests_) {
    if (!gg.showrooms.empty() && gg.showrooms[0]) resident_models.push_back(gid);
  }
  auto resident_sr = [&](int64_t mid) -> Showroom* {
    const auto it = guests_.find(mid);
    return (it != guests_.end() && !it->second.showrooms.empty() && it->second.showrooms[0])
               ? it->second.showrooms[0].get()
               : nullptr;
  };

  constexpr float kTwoPi = 6.28318530717958647692f;
  constexpr double kAsyncDelayDistSq = 140.0 * 140.0;
  const double tod_sec = population_->hour() * 3600.0;
  const bool tod_lamps = drive::CarLampState::tod_requests_headlamps(tod_sec);

  struct VisibleCar {
    const traffic::Car* car = nullptr;
    const drive::Vehicle* body = nullptr;
    int64_t render_model = 0;
    drive::Vec3 at{};
    drive::Quat rot{0, 0, 0, 1};
    double d2 = 1e30;
    double speed = 0.0;
    double steer = 0.0;
    float alpha = 1.0f;
    drive::LampShaderVectors lamps{};
    Showroom* assigned = nullptr;
    WheelPose wposes[4]{};
  };
  std::vector<VisibleCar> visible;
  visible.reserve(population_->cars().size());

  for (const auto& c : population_->cars()) {
    const float alpha = std::clamp(c.alpha, 0.0f, 1.0f);
    if (alpha <= 0.005f) continue;
    const drive::Vehicle* body = bodies_->body(c.id);
    TrafficPose pose{
        body ? body->position() : drive::Vec3{c.position.x, c.position.y, c.position.z},
        body ? body->orientation() : lane_orientation(c.heading, c.pitch),
    };
    if (blend_ < 1.0) {
      if (const auto bit = before_.find(c.id); bit != before_.end()) {
        pose = blend_traffic_pose(bit->second, pose, blend_);
      }
    }
    const drive::Vec3 at = pose.pos;
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) continue;
    drive::Quat q = pose.rot;
    const double qn = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(qn) || qn < 1e-6) q = {0, 0, 0, 1};
    else q = {q.x / qn, q.y / qn, q.z / qn, q.w / qn};

    const double dx = at.x - player_pos.x, dy = at.y - player_pos.y, dz = at.z - player_pos.z;
    const double d2 = dx * dx + dy * dy + dz * dz;

    int64_t render_model = 0;
    Showroom* primary_sr = nullptr;
    if (const auto lit = locked_model_.find(c.id); lit != locked_model_.end()) {
      render_model = lit->second;
      primary_sr = resident_sr(render_model);
      if (!primary_sr) continue;
    } else if (Showroom* own_sr = resident_sr(c.model)) {
      render_model = c.model;
      primary_sr = own_sr;
      locked_model_.emplace(c.id, render_model);
    } else {
      const auto git = guests_.find(c.model);
      const bool target_loading = (git != guests_.end() && git->second.loading.valid() && !git->second.failed);
      // Delay displaying a newly spawned far-away car until its own async model is resident;
      // if already within view or promoted to physics, lock permanently to a resident fallback
      // model so no visible car ever morphs mid-road.
      if (target_loading && d2 > kAsyncDelayDistSq && !c.physics) continue;
      if (resident_models.empty()) continue;
      render_model = resident_models[size_t(c.id) % resident_models.size()];
      primary_sr = resident_sr(render_model);
      if (!primary_sr) continue;
      locked_model_.emplace(c.id, render_model);
    }

    const bool is_new = !lamps_.count(c.id);
    drive::CarLampState& lstate = lamps_[c.id];
    drive::LampInputs lin;
    const bool decel_braking =
        (c.accel < -config_.brake_light_accel_threshold) || (c.speed > c.target + 1.0);
    const bool stopped_braking = body ? (body->speed() < 0.2) : (c.speed < 0.5 && c.target < 0.5);
    lin.brake_cmd = (decel_braking || stopped_braking) ? 1.0f : 0.0f;
    lin.brake_pedal = lin.brake_cmd;
    // drive::Vehicle::gear(): -1 = Reverse, 0 = Neutral, >= 1 = forward.
    // drive::LampInputs::gear: 0 = Reverse, >= 1 = non-reverse.
    lin.gear = body ? (body->gear() == -1 ? 0 : std::max(1, body->gear())) : 1;
    lin.tod_sec = tod_sec;
    lin.dt = lamp_dt;
    lin.indicator_mode = c.hazard_on ? 3 : (c.turn_left ? 1 : (c.turn_right ? 2 : 0));
    const bool car_in_tunnel =
        !tod_lamps && d2 < 220.0 * 220.0 && ground_ && *ground_ && is_under_tunnel_roof(*ground_, at);
    lin.light_override = c.flash_headlamps ? 2 : (car_in_tunnel ? 1 : 0);
    lin.is_ai = true;
    lin.snap_headlamps = is_new;
    lstate.update(lin);

    VisibleCar vc{};
    vc.car = &c;
    vc.body = body;
    vc.render_model = render_model;
    vc.at = at;
    vc.rot = q;
    vc.d2 = d2;
    vc.speed = body ? body->speed() : c.speed;
    vc.steer = body ? body->wheels()[0].steer : c.steer_angle;
    vc.alpha = alpha;
    vc.lamps = lstate.vectors();

    if (body) {
      for (int wi = 0; wi < 4; ++wi) {
        const auto& w = body->wheels()[size_t(wi)];
        vc.wposes[wi].centre[0] = float(w.centre.x);
        vc.wposes[wi].centre[1] = float(w.centre.y);
        vc.wposes[wi].centre[2] = float(w.centre.z);
        vc.wposes[wi].steer = float(w.steer + w.toe);
        vc.wposes[wi].spin = float(w.spin);
        vc.wposes[wi].omega = float(w.omega);
      }
      wheel_spin_[c.id] = vc.wposes[0].spin;
    } else {
      const auto& rest = primary_sr->car().wheel_centres_body;
      const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
      const float omega = float(vc.speed) / radius;
      float& spin = wheel_spin_[c.id];
      spin = std::fmod(spin + omega * lamp_dt, kTwoPi);
      if (spin < 0.0f) spin += kTwoPi;
      for (int wi = 0; wi < 4; ++wi) {
        vc.wposes[wi].centre[0] = rest[size_t(wi)][0];
        vc.wposes[wi].centre[1] = rest[size_t(wi)][1];
        vc.wposes[wi].centre[2] = rest[size_t(wi)][2];
        vc.wposes[wi].steer = (wi < 2) ? float(vc.steer) : 0.0f;
        vc.wposes[wi].spin = spin;
        vc.wposes[wi].omega = omega;
      }
    }
    visible.push_back(vc);
  }

  // Group visible cars by their permanently locked render_model and sort each group nearest-first
  // so the closest cars receive dedicated Showroom clones with independent wheel poses.
  std::map<int64_t, std::vector<size_t>> by_model;
  for (size_t i = 0; i < visible.size(); ++i) by_model[visible[i].render_model].push_back(i);

  for (auto& [mid, idxs] : by_model) {
    std::sort(idxs.begin(), idxs.end(),
              [&](size_t a, size_t b) { return visible[a].d2 < visible[b].d2; });
    auto git = guests_.find(mid);
    if (git == guests_.end() || git->second.showrooms.empty()) continue;
    const auto& slots = git->second.showrooms;
    const size_t n_slots = slots.size();
    struct SlotState {
      Showroom* sr = nullptr;
      double speed = 0.0;
      double steer = 0.0;
    };
    std::vector<SlotState> active_slots;
    active_slots.reserve(n_slots);
    for (size_t k = 0; k < idxs.size(); ++k) {
      VisibleCar& vc = visible[idxs[k]];
      if (k < n_slots && slots[k]) {
        Showroom* sr = slots[k].get();
        vc.assigned = sr;
        sr->set_wheel_poses(vc.wposes);
        active_slots.push_back({sr, vc.speed, vc.steer});
      } else if (!active_slots.empty()) {
        // Overflow distant car (> kMaxWheelClonesPerModel or while clone is async-loading):
        // pick the existing slot whose speed & steer match closest without clobbering its pose.
        size_t best_s = 0;
        double best_cost = 1e30;
        for (size_t s = 0; s < active_slots.size(); ++s) {
          const double cost = std::fabs(active_slots[s].speed - vc.speed) +
                              8.0 * std::fabs(active_slots[s].steer - vc.steer);
          if (cost < best_cost) {
            best_cost = cost;
            best_s = s;
          }
        }
        vc.assigned = active_slots[best_s].sr;
      }
    }
  }

  // Publish exact interpolated nearby traffic OBBs for chase-camera pull-in.
  publish_camera_obbs(player_pos);

  std::vector<Showroom::Instance> list;
  list.reserve(visible.size());
  for (const auto& vc : visible) {
    if (!vc.assigned) continue;
    Showroom* guest_sr = vc.assigned;
    // The guest's model frame to the world, relative to the player (the translation in double).
    float gm[16];
    drive::model_to_world(guest_sr->car(), vc.rot, {0, 0, 0}, gm);
    double gmd[16];
    to_double(gm, gmd);
    gmd[12] += vc.at.x, gmd[13] += vc.at.y, gmd[14] += vc.at.z;
    Showroom::Instance in;
    in.car = guest_sr;
    multiply(world_to_player, gmd, in.model);
    in.fade_alpha = vc.alpha;
    if (const auto& cols = guest_sr->car().colours; !cols.empty()) {
      const uint32_t rgb = cols[size_t(vc.car->id) % cols.size()].rgb;
      auto to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
      in.paint_rgb[0] = to_lin(float((rgb >> 16) & 0xFF) / 255.0f);
      in.paint_rgb[1] = to_lin(float((rgb >> 8) & 0xFF) / 255.0f);
      in.paint_rgb[2] = to_lin(float(rgb & 0xFF) / 255.0f);
      in.has_paint = true;
    }
    in.lamps = vc.lamps;
    in.has_lamps = true;
    list.push_back(in);
  }
  drawn_ = list.size();
  host_.set_instances(std::move(list));
}

void TrafficView::contact_bodies(std::vector<drive::ContactBody>& out, int group) {
  if (!bodies_) return;
  const double scale = bodies_->torque_scale();
  bodies_->for_each([&](drive::Vehicle& v) { out.push_back({&v, scale, group, drive::ContactRole::kTraffic}); });
}

TrafficView::RewindSnapshot TrafficView::capture_rewind() const {
  RewindSnapshot snap;
  if (population_) snap.population = population_->snapshot();
  if (bodies_) snap.bodies = bodies_->snapshot();
  snap.before = before_;
  snap.locked_model = locked_model_;
  return snap;
}

void TrafficView::restore_rewind(const RewindSnapshot& s) {
  if (population_) population_->restore(s.population);
  if (bodies_) bodies_->restore(s.bodies);
  before_ = s.before;
  locked_model_ = s.locked_model;
  lamps_at_ = population_ ? population_->time() : -1.0;
}

Showroom::Instance guest_instance_pose(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                                       Showroom& guest, const drive::Vec3& world_pos, const drive::Quat& world_rot) {
  float pm[16];
  drive::model_to_world(player_info, player, pm);
  double pmd[16], world_to_player[16];
  to_double(pm, pmd);
  invert_affine(pmd, world_to_player);
  float gm[16];
  drive::model_to_world(guest.car(), world_rot, {0, 0, 0}, gm);
  double gmd[16];
  to_double(gm, gmd);
  gmd[12] += world_pos.x, gmd[13] += world_pos.y, gmd[14] += world_pos.z;
  Showroom::Instance in;
  in.car = &guest;
  multiply(world_to_player, gmd, in.model);
  return in;
}

Showroom::Instance guest_instance(const cars::bake::CarInfo& player_info, const drive::Vehicle& player, Showroom& guest,
                                  const drive::Vehicle& body) {
  return guest_instance_pose(player_info, player, guest, body.position(), body.orientation());
}

std::string TrafficView::summary() const {
  if (!population_) return "traffic: off";
  const auto& st = population_->stats();
  char buf[320];
  std::snprintf(buf, sizeof buf,
                "traffic: %zu active (%zu bodies, %zu drawn) at %04.1f h, target %.2f; %d initial, %d spawned, %d "
                "despawned; %d contacts (%d with the player); %d models uploaded (longest %.0f ms)",
                population_->cars().size(), bodies_->count(), drawn_, population_->hour(), population_->target(),
                st.initial, st.spawned, st.despawned, bodies_->contacts(), bodies_->player_contacts(), uploads_,
                upload_ms_max_);
  return buf;
}

}  // namespace carview
