#include "traffic_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

}  // namespace

TrafficView::TrafficView(Renderer& renderer, Showroom& host, std::filesystem::path bake_dir, bool sync_loads)
    : r_(renderer), host_(host), bake_dir_(std::move(bake_dir)), sync_(sync_loads) {}

TrafficView::~TrafficView() {
  host_.set_instances({});
  for (auto& [id, g] : guests_)
    if (g.loading.valid()) g.loading.wait();
  guests_.clear();  // each guest idles the GPU before it frees anything
}

bool TrafficView::open(const std::filesystem::path& game_root, const drive::Ground* const* ground, double hour,
                       uint64_t seed, std::string& error) {
  game_root_ = game_root;
  if (!traffic::read_nav(game_root / "media" / "tracks" / "colorado" / "colorado.nav", map_, &error) ||
      !traffic::read_traffic_config(game_root, config_, &error))
    return false;
  roads_.build(map_);
  // The baked traffic models, by Data_Car.Id (the "car" line of the rows).
  std::error_code ec;
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
  if (media_.empty()) {
    error = "traffic: none of the freeroam traffic models is baked in " + bake_dir_.string();
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
      g.loading = std::async(std::launch::async, [r, bake, root, media]() {
        auto s = std::make_unique<Showroom>(*r);
        s->set_guest();
        std::string e;
        if (!s->load(bake, media, ShowroomOptions{}, e)) {
          std::fprintf(stderr, "carview: traffic model %s: %s\n", media.c_str(), e.c_str());
          s.reset();
        } else {
          s->load_car_lamps(root, media);
        }
        return s;
      });
    }
    if (!sync_ && g.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
    auto s = g.loading.get();
    if (!s) {
      g.failed = true;
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string e;
    if (!s->upload(e)) {
      std::fprintf(stderr, "carview: traffic model %s: %s\n", media_[id].c_str(), e.c_str());
      g.failed = true;
      continue;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    upload_ms_max_ = std::max(upload_ms_max_, ms);
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
  std::vector<Showroom::Instance> list;
  for (const auto& c : population_->cars()) {
    const auto it = guests_.find(c.model);
    if (it == guests_.end() || !it->second.showroom) continue;
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
    const drive::Quat q = pose.rot;
    const drive::Vec3 at = pose.pos;
    // The guest's model frame to the world, relative to the player (the
    // translation in double).
    float gm[16];
    drive::model_to_world(it->second.showroom->car(), q, {0, 0, 0}, gm);
    double gmd[16];
    to_double(gm, gmd);
    gmd[12] += at.x, gmd[13] += at.y, gmd[14] += at.z;
    Showroom::Instance in;
    in.car = it->second.showroom.get();
    multiply(world_to_player, gmd, in.model);
    drive::CarLampState lstate;
    lstate.headlamps_on = drive::CarLampState::tod_requests_headlamps(population_->hour() * 3600.0);
    lstate.brake_on = (c.speed > c.target + 1.0 || (body && body->speed() < 0.2));
    in.lamps = lstate.vectors();
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
