#include "traffic_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

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

// Per-guest kinematic wheel spin accumulator (rad) for traffic cars without a live physics body.
std::map<const Showroom*, float> g_traffic_wheel_spin;

}  // namespace

TrafficView::TrafficView(Renderer& renderer, Showroom& host, std::filesystem::path bake_dir, bool sync_loads)
    : r_(renderer), host_(host), bake_dir_(std::move(bake_dir)), sync_(sync_loads) {}

TrafficView::~TrafficView() {
  host_.set_instances({});
  for (auto& [id, g] : guests_)
    if (g.loading.valid()) g.loading.wait();
  r_.wait_idle();  // one single device idle on shutdown instead of one per guest
  for (auto& [id, g] : guests_)
    if (g.showroom) {
      g_traffic_wheel_spin.erase(g.showroom.get());
      g.showroom->skip_wait_idle_on_destroy();
    }
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

void TrafficView::step(drive::Vehicle& player) {
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
  population_->step(drive::Vehicle::kStep, {p.x, p.y, p.z}, {v.x, v.y, v.z});
  // The bodies' parameters come with the models: refresh them on the sim's
  // clock too, so a run that renders seldom (headless --every) still gets
  // bodies near the player.
  if (population_->time() - guests_at_ >= 0.25) {
    guests_at_ = population_->time();
    update_guests();
  }
  if (ground_ && *ground_) bodies_->step(**ground_, &player);
}

void TrafficView::update_guests() {
  // The models wanted: the loaded set and any with a car still driving.
  std::set<int64_t> want(population_->loaded().begin(), population_->loaded().end());
  for (const auto& c : population_->cars()) want.insert(c.model);
  for (int64_t id : want) {
    if (!media_.count(id)) continue;
    Guest& g = guests_[id];
    if (g.showroom || g.failed) continue;
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
    if (!sync_ && g.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
    Loaded loaded = g.loading.get();
    auto s = std::move(loaded.showroom);
    if (!s) {
      g.failed = true;
      continue;
    }
    if (!loaded.error.empty()) {
      std::fprintf(stderr, "carview: traffic model %s: %s\n", media_[id].c_str(), loaded.error.c_str());
      g.failed = true;
      continue;
    }
    double ms = loaded.upload_ms;
    if (!loaded.uploaded) {
      const auto t0 = std::chrono::steady_clock::now();
      std::string e;
      if (!s->upload(e)) {
        std::fprintf(stderr, "carview: traffic model %s: %s\n", media_[id].c_str(), e.c_str());
        g.failed = true;
        continue;
      }
      ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    upload_ms_max_ = std::max(upload_ms_max_, loaded.uploaded ? 0.0 : ms);  // render-thread stall only
    ++uploads_;
    params_[id] = drive::params_for_baked_car(s->car(), bake_dir_);
    g.showroom = std::move(s);
    std::printf("traffic: t %.1f s: model %lld (%s) uploaded in %.0f ms\n", population_->time(), (long long)id,
                media_[id].c_str(), ms);
  }
  // Models neither loaded nor driving: dropped (out of the instances first).
  for (auto it = guests_.begin(); it != guests_.end();) {
    if (want.count(it->first) || it->second.loading.valid()) {
      ++it;
      continue;
    }
    host_.set_instances({});
    params_.erase(it->first);
    std::printf("traffic: t %.1f s: model %lld (%s) dropped\n", population_->time(), (long long)it->first,
                media_[it->first].c_str());
    // Retire dropped traffic GPU resources through the host's multi-frame retirement ring
    // instead of calling vkDeviceWaitIdle() on the render thread (eliminates 4.5-13.1 ms hitches).
    if (it->second.showroom) {
      g_traffic_wheel_spin.erase(it->second.showroom.get());
      std::shared_ptr<Showroom> retired = std::move(it->second.showroom);
      retired->skip_wait_idle_on_destroy();
      host_.retire_deferred([g = std::move(retired)]() mutable { g.reset(); });
    }
    it = guests_.erase(it);
  }
}

void TrafficView::place(const cars::bake::CarInfo& player_info, const drive::Vehicle& player) {
  if (!population_) return;
  update_guests();
  float pm[16];
  drive::model_to_world(player_info, player, pm);
  double pmd[16], world_to_player[16];
  to_double(pm, pmd);
  invert_affine(pmd, world_to_player);
  const double now_t = population_->time();
  const float lamp_dt = lamps_at_ >= 0 ? float(std::clamp(now_t - lamps_at_, 0.0, 0.25)) : float(drive::Vehicle::kStep);
  lamps_at_ = now_t;
  std::set<uint32_t> alive_ids;
  for (const auto& c : population_->cars()) alive_ids.insert(c.id);
  for (auto it = lamps_.begin(); it != lamps_.end();)
    it = alive_ids.count(it->first) ? std::next(it) : lamps_.erase(it);
  // Fallback to any already-loaded traffic guest so an active traffic car whose own model
  // is still async-loading or failed never drives around as an invisible obstacle.
  Showroom* fallback_guest = nullptr;
  for (const auto& [gid, gg] : guests_) {
    if (gg.showroom) {
      fallback_guest = gg.showroom.get();
      break;
    }
  }
  struct WheelDriver {
    double dist2 = 1e30;
    const drive::Vehicle* body = nullptr;
    double speed = 0.0;
  };
  std::map<Showroom*, WheelDriver> wheel_drivers;
  const drive::Vec3 player_pos = player.position();
  std::vector<Showroom::Instance> list;
  for (const auto& c : population_->cars()) {
    const float alpha = std::clamp(c.alpha, 0.0f, 1.0f);
    if (alpha <= 0.005f) continue;
    const auto it = guests_.find(c.model);
    Showroom* guest_sr = (it != guests_.end() && it->second.showroom) ? it->second.showroom.get() : fallback_guest;
    if (!guest_sr) continue;
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
    // The guest's model frame to the world, relative to the player (the
    // translation in double).
    float gm[16];
    drive::model_to_world(guest_sr->car(), q, {0, 0, 0}, gm);
    double gmd[16];
    to_double(gm, gmd);
    gmd[12] += at.x, gmd[13] += at.y, gmd[14] += at.z;
    Showroom::Instance in;
    in.car = guest_sr;
    multiply(world_to_player, gmd, in.model);
    in.fade_alpha = alpha;
    const bool is_new = !lamps_.count(c.id);
    drive::CarLampState& lstate = lamps_[c.id];
    drive::LampInputs lin;
    lin.brake_cmd = (c.speed > c.target + 1.0 || (body && body->speed() < 0.2)) ? 1.0f : 0.0f;
    lin.brake_pedal = lin.brake_cmd;
    lin.gear = body ? body->gear() : 1;
    lin.tod_sec = population_->hour() * 3600.0;
    lin.dt = lamp_dt;
    lin.indicator_mode = c.hazard_on ? 3 : (c.turn_left ? 1 : (c.turn_right ? 2 : 0));
    lin.light_override = c.flash_headlamps ? 2 : 0;
    lin.is_ai = true;
    lin.snap_headlamps = is_new;
    lstate.update(lin);
    in.lamps = lstate.vectors();
    in.has_lamps = true;
    list.push_back(in);

    // Track nearest visible car of each guest model to drive its wheel spin & steer poses.
    const double dx = at.x - player_pos.x, dy = at.y - player_pos.y, dz = at.z - player_pos.z;
    const double d2 = dx * dx + dy * dy + dz * dz;
    auto& wd = wheel_drivers[guest_sr];
    if (d2 < wd.dist2) {
      wd.dist2 = d2;
      wd.body = body;
      wd.speed = body ? body->speed() : c.speed;
    }
  }
  constexpr float kTwoPi = 6.28318530717958647692f;
  for (const auto& [guest_sr, wd] : wheel_drivers) {
    WheelPose wposes[4]{};
    if (wd.body) {
      for (int wi = 0; wi < 4; ++wi) {
        const auto& w = wd.body->wheels()[size_t(wi)];
        wposes[wi].centre[0] = float(w.centre.x);
        wposes[wi].centre[1] = float(w.centre.y);
        wposes[wi].centre[2] = float(w.centre.z);
        wposes[wi].steer = float(w.steer);
        wposes[wi].spin = float(w.spin);
        wposes[wi].omega = float(w.omega);
      }
      g_traffic_wheel_spin[guest_sr] = wposes[0].spin;
    } else {
      const auto& rest = guest_sr->car().wheel_centres_body;
      const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
      const float omega = float(wd.speed) / radius;
      float& spin = g_traffic_wheel_spin[guest_sr];
      spin = std::fmod(spin + omega * lamp_dt, kTwoPi);
      if (spin < 0.0f) spin += kTwoPi;
      for (int wi = 0; wi < 4; ++wi) {
        wposes[wi].centre[0] = rest[size_t(wi)][0];
        wposes[wi].centre[1] = rest[size_t(wi)][1];
        wposes[wi].centre[2] = rest[size_t(wi)][2];
        wposes[wi].steer = 0.0f;
        wposes[wi].spin = spin;
        wposes[wi].omega = omega;
      }
    }
    guest_sr->set_wheel_poses(wposes);
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
  return snap;
}

void TrafficView::restore_rewind(const RewindSnapshot& s) {
  if (population_) population_->restore(s.population);
  if (bodies_) bodies_->restore(s.bodies);
  before_ = s.before;
  lamps_at_ = population_ ? population_->time() : -1.0;
}

Showroom::Instance guest_instance(const cars::bake::CarInfo& player_info, const drive::Vehicle& player, Showroom& guest,
                                  const drive::Vehicle& body) {
  float pm[16];
  drive::model_to_world(player_info, player, pm);
  double pmd[16], world_to_player[16];
  to_double(pm, pmd);
  invert_affine(pmd, world_to_player);
  float gm[16];
  drive::model_to_world(guest.car(), body.orientation(), {0, 0, 0}, gm);
  double gmd[16];
  to_double(gm, gmd);
  const drive::Vec3 at = body.position();
  gmd[12] += at.x, gmd[13] += at.y, gmd[14] += at.z;
  Showroom::Instance in;
  in.car = &guest;
  multiply(world_to_player, gmd, in.model);
  return in;
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
