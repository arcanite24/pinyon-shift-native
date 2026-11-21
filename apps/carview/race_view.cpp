#include "race_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>

#include "drive/from_physics.h"
#include "race/street_line.h"
#include "traffic/nav.h"
#include "traffic_view.h"
#include "world/collision.h"

namespace carview {
namespace {

void clock_text(double t, char* out, size_t n) {
  if (t < 0) t = 0;
  const int m = int(t / 60);
  std::snprintf(out, n, "%d:%05.2f", m, t - 60 * m);
}

const char* ordinal(int p) {
  static const char* kNames[] = {"", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH"};
  return p >= 1 && p <= 8 ? kNames[p] : "";
}

}  // namespace

RaceView::RaceView(std::filesystem::path bake_dir) : bake_dir_(std::move(bake_dir)) {}
RaceView::~RaceView() = default;

bool RaceView::open(const std::filesystem::path& root, const std::string& event, const Options& o, std::string& error) {
  o_ = o;
  std::string e;
  replay_.clear();
  if (!o.replay.empty()) {
    std::ifstream f(o.replay);
    if (!f) {
      error = "cannot read " + o.replay.string();
      return false;
    }
    std::string line;
    std::getline(f, line);  // the header
    while (std::getline(f, line)) {
      double v[8] = {};
      if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                      &v[7]) == 8)
        replay_.push_back({v[0], v[1], v[2], v[3], v[4] * std::numbers::pi / 180, v[7]});
    }
    if (replay_.size() < 2) {
      error = o.replay.string() + ": no rows";
      return false;
    }
  }
  if (!race::load_event(root, event, o.difficulty, event_, &e) || !race::load_route(root, event_.route_id, route_, &e)) {
    error = e;
    return false;
  }
  // No AI line but checkpoints: a street or nemesis race on a line along
  // the road map (race/street_line.h; ours until Q47).
  street_ = false;
  if (!race::load_owt(root, event_.route_id, owt_, &e)) {
    if (race::checkpoints(route_).empty()) {
      error = e;
      return false;
    }
    traffic::NavMap nav;
    std::string ne;
    if (!traffic::read_nav(root / "media" / "tracks" / "colorado" / "colorado.nav", nav, &ne) ||
        !race::build_street_line(nav, route_, owt_, &ne)) {
      error = ne;
      return false;
    }
    street_ = true;
  }
  // Ribbon-0 lines are closed: circuits; open lines: one-lap sprints.
  circuit_ = owt_.closed;
  const int laps = circuit_ ? event_.laps : 1;
  line_ = race::build_race_line(owt_, &route_, circuit_ ? laps + 3 : 1);
  ribbon_ = std::make_unique<race::RibbonGround>(owt_);
  auto gate = route_.start_line();
  auto finish = route_.sprint_finish();
  const auto slots = route_.slots();
  if (street_ && !slots.empty()) {
    // The finish: the last checkpoint's plane, normal from the one before
    // (the oracle's measured rule); the start line (unused by a sprint):
    // through slot 00 along its facing; the grid stands behind pole.
    const auto cps = race::checkpoints(route_);
    finish = race::checkpoint_gate(route_, cps.size() - 1);
    const auto* s0 = slots[0];
    gate = race::Gate{s0->x, s0->z, s0->fx, s0->fz, 60};
    line_.gate_progress = line_.line.nearest(s0->x, s0->z, 0, std::min(400.0, line_.line.length())) + 5;
    double from = line_.gate_progress;
    for (size_t k = 0; k < cps.size(); ++k)
      if (auto g = race::checkpoint_gate(route_, k)) {
        checkpoint_gates_.push_back(*g);
        from = line_.line.nearest(g->x, g->z, from, std::min(line_.line.length(), from + 2000));
        checkpoint_progress_.push_back(from);
      }
  }
  if (!street_ && !circuit_ && !slots.empty() && (!gate || !finish)) {
    // An open line with no gantry or cannons (route 136): the start line
    // through slot 00; the finish across the AI line where it passes
    // nearest the last route_waypoint after the grid (ours; SPEC-EVENTS
    // names no rule, SPEC_QUESTIONS Q48).
    if (!gate) {
      const auto* s0 = slots[0];
      gate = race::Gate{s0->x, s0->z, s0->fx, s0->fz, 60};
      line_.gate_progress = line_.line.nearest(s0->x, s0->z, 0, line_.line.length()) + 5;
    }
    const race::RouteTransform* last = nullptr;
    for (int k = 0; k < 100; ++k) {
      char n[32];
      std::snprintf(n, sizeof n, "route_waypoint_%02d", k);
      if (const auto* t = route_.find(n)) last = t;
      else break;
    }
    if (last && !finish) {
      const double p = line_.line.nearest(last->x, last->z, line_.gate_progress + 200, line_.line.length());
      double x, y, z, h;
      line_.line.pose(p, 0, x, y, z, h);
      finish = race::Gate{x, z, std::sin(h), std::cos(h), 40};
    }
  }
  if (!gate || slots.empty() || (!circuit_ && !finish)) {
    error = "route " + std::to_string(event_.route_id) + ": no start line, slots or finish";
    return false;
  }
  for (const auto* s : slots) slots_.push_back(*s);
  const int cars = std::min(int(slots.size()), int(event_.opponents.size()) + 1);
  player_slot_ = race::player_slot(event_.player_grid_position, cars);
  const auto opp = race::opponent_slots(player_slot_, cars - 1);
  session_ = std::make_unique<race::Session>(size_t(cars), circuit_, laps, *gate, finish);
  field_ = std::make_unique<drive::AiField>(line_.line, size_t(cars),
                                            drive::AiField::Options{.full_gas = true, .off_line_slow = 0.08f});
  const double from = circuit_ ? line_.gate_progress - 0.5 * line_.lap_length : std::max(0.0, line_.gate_progress - 1500);
  cars_.clear();
  cars_.resize(size_t(cars - 1));
  for (size_t k = 0; k < cars_.size(); ++k) {
    Car& c = cars_[k];
    c.who = event_.opponents[k];
    c.slot = opp[k];
    c.setup = race::assign_driver(event_.event_skill, event_.event_temperament, event_.event_rubberband,
                                  c.who.skill_modifier, c.who.aggro_modifier, c.who.rubberband_modifier, event_);
    const race::SkillRow* sk = event_.skill(c.setup.skill);
    c.ai.init(sk ? *sk : race::SkillRow{}, event_.rubberband(c.setup.rubberband_id), o.seed * 1000003u + k, o.rubberband);
    const auto& s = slots_[size_t(c.slot)];
    field_->place_near(k, s.x, s.z, from, line_.gate_progress + 1);
    c.start_progress = field_->progress(k);
  }
  {
    const race::SkillRow* sk = event_.skill(std::clamp(event_.event_skill, 1, 80));
    player_ai_.init(sk ? *sk : race::SkillRow{}, nullptr, o.seed * 7919u, false);
    const auto& s = slots_[size_t(player_slot_)];
    field_->place_near(cars_.size(), s.x, s.z, from, line_.gate_progress + 1);
    player_start_ = field_->progress(cars_.size());
  }
  if (finish) finish_progress_ = line_.line.nearest(finish->x, finish->z, line_.gate_progress, line_.line.length());
  const size_t n = size_t(cars);
  states_.resize(n), inputs_.resize(n), progress_.resize(n), cross_.resize(n), speed_.resize(n), lanes_.assign(n, 0), caps_.assign(n, 1e9f);
  samples_.resize(n);
  lane_params_.resize(n);
  for (size_t k = 0; k < cars_.size(); ++k) {
    if (const auto* t = event_.temperament(cars_[k].setup.temperament))
      lane_params_[k] = {float(t->trailing_distance), float(t->car_clearance), float(t->edge_clearance)};
  }
  std::printf("race: %s, route %d (%s, %d lap%s, line %.0f m a lap), %d cars, player on slot %02d, %zu ribbon "
              "triangles\n",
              event.c_str(), event_.route_id, circuit_ ? "circuit" : "sprint", laps, laps == 1 ? "" : "s",
              line_.lap_length, cars, player_slot_, ribbon_->triangles());
  for (size_t k = 0; k < cars_.size(); ++k)
    std::printf("race:   slot %02d %-28s skill %d temperament %d rubber band %d\n", cars_[k].slot,
                cars_[k].who.car_media.c_str(), cars_[k].setup.skill, cars_[k].setup.temperament,
                cars_[k].setup.rubberband_id);
  return true;
}

void RaceView::player_spawn(double out[4]) const {
  const auto& s = slots_[size_t(player_slot_)];
  out[0] = s.x, out[1] = s.y, out[2] = s.z, out[3] = std::atan2(s.fx, s.fz);
}

void RaceView::opponent_positions(std::vector<std::array<double, 2>>& out) const {
  out.clear();
  for (const auto& c : cars_)
    if (c.body) out.push_back({c.body->position().x, c.body->position().z});
}

bool RaceView::post_race_spawn(double out[4]) const {
  const race::RouteTransform* t = route_.find("post_race_location_00");
  if (!t) return false;
  out[0] = t->x, out[1] = t->y, out[2] = t->z, out[3] = std::atan2(t->fx, t->fz);
  return true;
}

bool RaceView::load_cars(Renderer& renderer, const drive::Ground& ground, std::string& error) {
  for (auto& c : cars_) {
    auto it = models_.find(c.who.car_media);
    if (it == models_.end()) {
      auto m = std::make_unique<Showroom>(renderer);
      m->set_guest();
      std::string e;
      if (!m->load(bake_dir_, c.who.car_media, ShowroomOptions{}, e) || !m->upload(e)) {
        error = c.who.car_media + ": " + e;
        return false;
      }
      it = models_.emplace(c.who.car_media, std::move(m)).first;
    }
    c.model = it->second.get();
    std::vector<std::string> notes;
    c.body = std::make_unique<drive::Vehicle>(drive::params_for_baked_car(c.model->car(), bake_dir_, &notes));
    c.body->set_ai_controls(true);
    {
      // The title's AI cars weigh 0.98 of the stock car (SPEC-RACE-AI,
      // the measured tables); the tables take that mass, the physics the
      // car's own.
      drive::CarParams ai = c.body->params();
      ai.mass *= 0.98;
      c.capacity = drive::ai_capacity(ai);
    }
    c.body->set_ai_dirt_grip(true);
    const auto& s = slots_[size_t(c.slot)];
    double y = s.y;
    double gy = y;
    if (world::ground_height(ground, s.x, s.z, y + 5, gy)) y = gy;
    c.body->reset({s.x, y + 0.05, s.z}, std::atan2(s.fx, s.fz));
    c.px = s.x, c.pz = s.z;
  }
  // The ribbon against the collision: heights at the line's centre points
  // within 300 m of the player's slot (a car handed between the two should
  // not drop or climb).
  {
    const auto& ps = slots_[size_t(player_slot_)];
    std::vector<double> dy;
    for (size_t i = 0; i < owt_.points.size(); i += 5) {
      const auto& q = owt_.points[i];
      if (std::hypot(q.x - ps.x, q.z - ps.z) > 300) continue;
      double a = 0, b = 0;
      if (world::ground_height(ground, q.x, q.z, q.y + 10, a) && world::ground_height(*ribbon_, q.x, q.z, q.y + 10, b))
        dy.push_back(b - a);
    }
    if (!dy.empty()) {
      std::sort(dy.begin(), dy.end());
      std::printf("race: ribbon minus collision height at %zu line points: median %.2f m, %.2f to %.2f m\n", dy.size(),
                  dy[dy.size() / 2], dy.front(), dy.back());
    }
  }
  return true;
}

void RaceView::release_models() {
  for (auto& c : cars_) c.model = nullptr;
  models_.clear();
}

drive::Input RaceView::player_input(const drive::Vehicle& player, const drive::Input& user) {
  const size_t n = cars_.size();
  const drive::Vec3 pp = player.position();
  ppx_ = pp.x, ppz_ = pp.z;
  if (!player_capacity_known_) {
    player_capacity_ = drive::ai_capacity(player.params());
    player_capacity_known_ = true;
    const char* e = std::getenv("SHIFT_RACE_PLANNER");
    planner_fit_ = e && std::string(e) == "fit";
    const auto& pc = player_capacity_;
    std::printf("race: planner %s; the player's car plans lateral %.1f, braking %.1f (x %.2f) m/s^2 at 20 m/s\n",
                planner_fit_ ? "fit (1.7 g, 2.0 g)" : "SPEC-RACE-AI tables", pc.lateral[8], pc.braking[8],
                pc.braking_k);
    for (const auto& c : cars_)
      std::printf("race:   %-28s lateral %.1f braking %.1f m/s^2 at 20 m/s\n", c.who.car_media.c_str(),
                  c.capacity.lateral[8], c.capacity.braking[8]);
  }
  for (size_t k = 0; k < n; ++k) {
    states_[k] = drive::ai_state(*cars_[k].body);
    const float C = float(cars_[k].ai.cornering()), B = float(cars_[k].ai.braking());
    if (planner_fit_) field_->set_limits(k, float(race::corner_accel(C)), float(race::brake_decel(B)));
    else field_->set_planner(k, &cars_[k].capacity, C, B);
  }
  states_[n] = drive::ai_state(player);
  {
    const float C = float(player_ai_.cornering()), B = float(player_ai_.braking());
    if (planner_fit_) field_->set_limits(n, float(race::corner_accel(C)), float(race::brake_decel(B)));
    else field_->set_planner(n, &player_capacity_, C, B);
  }
  // Lanes from the last step's progress, then the field's pass.
  for (size_t k = 0; k <= n; ++k)
    progress_[k] = field_->progress(k), cross_[k] = field_->cross_track(k), speed_[k] = states_[k].speed;
  race::plan_lanes(line_.line, progress_, cross_, speed_, lane_params_, n, lanes_, drive::Vehicle::kStep, 1.5, caps_);
  for (size_t k = 0; k < n; ++k) field_->set_lane(k, lanes_[k]), field_->set_speed_cap(k, caps_[k]);
  field_->drive(states_, inputs_);
  // Held on the grid in first: brake under the reverse-select press (0.5) and the handbrake.
  if (!session_->started()) return drive::Input{0, 0, 0.4, true};
  if (o_.autopilot || session_->finished(n)) return inputs_[n];
  return user;
}

void RaceView::replay_player(drive::Vehicle& player) const {
  if (replay_.empty() || !session_->started()) return;
  const double t = session_->clock() + drive::Vehicle::kStep;  // the clock after this step
  if (t > replay_.back().t) return;
  auto it = std::lower_bound(replay_.begin(), replay_.end(), t, [](const ReplayRow& r, double v) { return r.t < v; });
  if (it == replay_.begin()) ++it;
  const ReplayRow& a = *(it - 1);
  const ReplayRow& b = *it;
  const double u = b.t > a.t ? std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0) : 0.0;
  double dh = b.heading - a.heading;
  while (dh > std::numbers::pi) dh -= 2 * std::numbers::pi;
  while (dh < -std::numbers::pi) dh += 2 * std::numbers::pi;
  const double h = a.heading + u * dh, speed = a.speed + u * (b.speed - a.speed);
  // The body on the path, level, moving along its heading (a kinematic
  // stand-in: its gaps, not its physics, are what the gate needs).
  const drive::Vec3 at{a.x + u * (b.x - a.x), a.y + u * (b.y - a.y), a.z + u * (b.z - a.z)};
  player.place(at, h);
  player.set_velocity(drive::Vec3{speed * std::sin(h), 0, speed * std::cos(h)}, drive::Vec3{0, 0, 0});
}

void RaceView::reset_car(drive::Vehicle& v, size_t i, const drive::Ground& near, double cx, double cz, double radius) {
  const double p = field_->progress(i);
  double x, y, z, h;
  line_.line.pose(p, 0, x, y, z, h);
  const bool inside = std::hypot(x - cx, z - cz) < radius - 40;
  const drive::Ground& g = inside ? near : static_cast<const drive::Ground&>(*ribbon_);
  if (i < cars_.size()) cars_[i].far = !inside;
  double gy = y;
  if (world::ground_height(g, x, z, y + 5, gy)) y = gy;
  const double speed = 0.5 * field_->planned_speed(i, p);
  v.launch({x, y + 0.05, z}, h, speed);
  field_->set_lane(i, 0);
  if (i < lanes_.size()) lanes_[i] = 0;
  session_->reset_done(i);
  ++resets_;
  std::printf("race: t %.2f s: %s reset to the line at %.0f m (%.1f m/s; %s)\n", session_->clock(), name_of(i).c_str(),
              p, speed, session_->car(i).reason);
}

void RaceView::after_step(drive::Vehicle& player, const drive::Ground& near, double cx, double cz, double radius,
                          TrafficView* traffic) {
  const size_t n = cars_.size();
  const double clock = session_->clock() + drive::Vehicle::kStep;  // the session's clock after this step
  // The player's car under the AI: the AI's controls and grip on dirt.
  const bool player_ai = o_.autopilot || session_->finished(n);
  if (player.ai_controls() != player_ai) {
    player.set_ai_controls(player_ai);
    player.set_ai_dirt_grip(player_ai);
  }
  const double grid_cars = double(n + 1);
  for (size_t k = 0; k < n; ++k) {
    Car& c = cars_[k];
    const drive::Vec3 at = c.body->position();
    c.px = at.x, c.pz = at.z;
    drive::Input in = inputs_[k];
    // Held on the grid; revving from 3.09 s before the signal, clutch dropped at it.
    if (clock < 0) in = clock >= -3.09 ? drive::Input{0, 1, 1, true} : drive::Input{0, 0, 0.4, true};
    else drive::ai_traction(*c.body, c.traction_cap, drive::Vehicle::kStep, in);
    const double boost = race::start_boost(c.slot, int(grid_cars), field_->progress(k) - c.start_progress);
    c.body->set_torque_factor(c.ai.torque() * (1 + boost));
    // The gathered collision covers `radius` round (cx, cz); outside it
    // (with a margin for the car and some hysteresis) the ribbon.
    const double d = std::hypot(at.x - cx, at.z - cz);
    if (c.far && d < radius - 80) c.far = false;
    else if (!c.far && d > radius - 40) c.far = true;
    // Near but with no collision under the car (a hole in the strips, or
    // a gather still on its way): the ribbon for this step.
    bool ribbon = c.far;
    if (!ribbon) {
      double gy = 0;
      ribbon = !world::ground_height(near, at.x, at.z, at.y + 3, gy) || gy < at.y - 10;
      hole_steps_ += ribbon;
    }
    far_steps_ += ribbon;
    c.body->step(in, ribbon ? static_cast<const drive::Ground&>(*ribbon_) : near);
  }
  // Contacts: the player and traffic in group 1, the field in 0; a car
  // just reset touches nothing for 3 s.
  contacts_.clear();
  if (session_->collides(n)) contacts_.push_back({&player, 1.0, 1});
  for (size_t k = 0; k < n; ++k)
    if (session_->collides(k)) contacts_.push_back({cars_[k].body.get(), 1.0, 0});
  if (traffic) traffic->contact_bodies(contacts_, 1);
  contact_count_ += drive::resolve_contacts(contacts_, {});
  // The session: each car's move over the step.
  auto sample = [&](size_t i, const drive::Vehicle& v, double x0, double z0) {
    race::CarSample& s = samples_[i];
    const drive::Vec3 p = v.position();
    s.x0 = x0, s.z0 = z0, s.x1 = p.x, s.z1 = p.z;
    s.speed = v.speed();
    s.progress = field_->progress(i);
    double lx, ly, lz, h;
    line_.line.pose(s.progress, 0, lx, ly, lz, h);
    const drive::AiCarState st = drive::ai_state(v);
    s.heading_cos = st.fwd_x * std::sin(h) + st.fwd_z * std::cos(h);
    const double cr = field_->cross_track(i);
    const double room = line_.line.room(s.progress, cr > 0);
    s.off_route = std::max(0.0, std::fabs(cr) - room);
  };
  for (size_t k = 0; k < n; ++k) sample(k, *cars_[k].body, cars_[k].px, cars_[k].pz);
  sample(n, player, ppx_, ppz_);
  // Checkpoints in order (SPEC-EVENTS, Pass rule); the last is the finish,
  // held until every earlier one is passed. A missed one is not counted
  // past (interim: the off-route reset brings the car back).
  if (checkpoint_gates_.size() > 1) {
    next_checkpoint_.resize(n + 1, 0);
    const size_t last = checkpoint_gates_.size() - 1;
    for (size_t i = 0; i <= n; ++i) {
      const auto& q = samples_[i];
      size_t& k = next_checkpoint_[i];
      // Crossed within the gate's reach, or (ours, the missed-checkpoint
      // rule is open) carried past it along the line: a reset puts a car
      // back on the line ahead, and a wide car can pass outside the reach.
      if (session_->started() && k < last &&
          (checkpoint_gates_[k].crossing(q.x0, q.z0, q.x1, q.z1) >= 0 || q.progress > checkpoint_progress_[k] + 10))
        ++k;
      session_->hold_finish(i, k < last);
    }
  }
  session_->step(drive::Vehicle::kStep, samples_);
  // SHIFT_RACE_TRACE=<file>: every car 4 times a second (JSON lines).
  static FILE* trace = [] {
    const char* p = std::getenv("SHIFT_RACE_TRACE");
    return p ? std::fopen(p, "w") : nullptr;
  }();
  static long trace_step = 0;
  if (trace && trace_step++ % 24 == 0) {  // every 0.2 s
    for (size_t i = 0; i <= n; ++i) {
      const drive::Vehicle& v = i < n ? *cars_[i].body : player;
      const auto& q = samples_[i];
      std::fprintf(trace,
                   "{\"t\":%.3f,\"car\":%zu,\"x\":%.2f,\"z\":%.2f,\"v\":%.2f,\"p\":%.1f,\"cross\":%.2f,\"lane\":%.2f,"
                   "\"target\":%.2f,\"thr\":%.2f,\"brk\":%.2f,\"steer\":%.2f,\"off\":%.2f,\"cos\":%.2f,\"gear\":%d,"
                   "\"far\":%d,\"state\":%d,\"torque\":%.3f}\n",
                   session_->clock(), i, q.x1, q.z1, q.speed, q.progress, field_->cross_track(i), lanes_[i],
                   field_->target_speed(i), inputs_[i].throttle, inputs_[i].brake, inputs_[i].steer, q.off_route,
                   q.heading_cos, v.gear(), i < n ? int(cars_[i].far) : 0, i < n ? int(cars_[i].ai.state()) : -1,
                   v.torque_factor());
    }
    std::fflush(trace);
  }
  // Rubber bands: the gap to the player along the line and the places.
  const int player_place = session_->car(n).place;
  for (size_t k = 0; k < n; ++k)
    cars_[k].ai.update(field_->progress(k) - field_->progress(n), session_->car(k).place - player_place);
  for (size_t i : session_->resets()) reset_car(i < n ? *cars_[i].body : player, i, near, cx, cz, radius);
  for (size_t i = 0; i <= n; ++i)
    if (session_->finished(i) && session_->car(i).finish > session_->clock() - drive::Vehicle::kStep - 1e-9)
      std::printf("race: t %.2f s: %s finished %s in %.3f s\n", session_->clock(), name_of(i).c_str(),
                  ordinal(session_->car(i).place), session_->car(i).finish);
}

void RaceView::instances(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                         std::vector<Showroom::Instance>& out) const {
  for (const auto& c : cars_)
    if (c.model && c.body) out.push_back(guest_instance(player_info, player, *c.model, *c.body));
}

std::string RaceView::name_of(size_t i) const {
  if (i >= cars_.size()) return "player";
  return "AI " + std::to_string(cars_[i].who.ai_player_id) + " (" + cars_[i].who.car_media + ")";
}

std::string RaceView::banner() const {
  const size_t n = cars_.size();
  const auto& s = *session_;
  char b[160], t[32];
  const double c = s.clock();
  if (c < -3) std::snprintf(b, sizeof b, "%s - %s - %zu cars", event_.horizon_id.c_str(),
                            circuit_ ? (std::to_string(s.laps()) + " laps").c_str() : "sprint", n + 1);
  else if (c < 0) std::snprintf(b, sizeof b, "%d", s.digit());
  else if (s.finished(n)) {
    clock_text(s.car(n).finish, t, sizeof t);
    const int p = s.car(n).place;
    const int64_t credits = award_ ? award_->credits : credits_for(p);
    if (award_ && award_->wristband_points > 0)
      std::snprintf(b, sizeof b, "FINISHED %s PLACE  %s  %lld CR  +%d WRISTBAND", ordinal(p), t, (long long)credits,
                    award_->wristband_points);
    else std::snprintf(b, sizeof b, "FINISHED %s PLACE  %s  %lld CR", ordinal(p), t, (long long)credits);
  } else {
    clock_text(c, t, sizeof t);
    const char* go = c < 1.5 ? "GO!  " : "";
    if (circuit_)
      std::snprintf(b, sizeof b, "%sLAP %d/%d  P%d/%zu  %s", go, s.lap(n), s.laps(), s.car(n).place, n + 1, t);
    else {
      // A sprint, and any race on an open line: PROGRESS in percent of the
      // run (SPEC-EVENTS, Laps on an open route).
      const double run = std::max(1.0, finish_progress_ - line_.gate_progress);
      const double pc = std::clamp((field_->progress(n) - line_.gate_progress) / run, 0.0, 1.0);
      std::snprintf(b, sizeof b, "%sP%d/%zu  %s  PROGRESS %.0f%%", go, s.car(n).place, n + 1, t, 100 * pc);
    }
  }
  return b;
}

int64_t RaceView::credits_for(int place) const {
  return race::race_credits(event_.cash_prize, race::winnings_for_career_type(event_.career_type),
                            int(session_->size()), place, true);
}

const race::RaceAward* RaceView::award(race::CareerRecord& career, const race::WristbandTables& tables,
                                       double top_speed) {
  const size_t n = cars_.size();
  if (award_ || !session_->finished(n)) return nullptr;
  const auto& c = session_->car(n);
  award_ = race::award_race(career, event_.event_id, event_.race_id, event_.career_type, event_.style, event_.level,
                            event_.cash_prize, int(session_->size()), c.place, true, c.finish, top_speed,
                            event_.prize_car, tables);
  return &*award_;
}

std::string RaceView::results() const {
  std::string out;
  const auto ord = session_->order();
  char line[200], t[32], best[32];
  for (size_t k = 0; k < ord.size(); ++k) {
    const size_t i = ord[k];
    const auto& c = session_->car(i);
    if (c.finish >= 0) clock_text(c.finish, t, sizeof t);
    else std::snprintf(t, sizeof t, "--");
    if (c.best_lap > 0) clock_text(c.best_lap, best, sizeof best);
    else std::snprintf(best, sizeof best, "--");
    const int slot = i < cars_.size() ? cars_[i].slot : player_slot_;
    const int64_t credits = credits_for(int(k) + 1);
    std::snprintf(line, sizeof line, "  P%zu  %-9s best %-9s slot %02d  %5.0f m  %s%s\n", k + 1, t, best, slot,
                  c.distance, name_of(i).c_str(), i == cars_.size() ? (" " + std::to_string(credits) + " CR").c_str() : "");
    out += line;
  }
  return out;
}

std::string RaceView::summary() const {
  char b[240];
  std::snprintf(b, sizeof b,
                "race: %d car-to-car contacts, %d resets, %.1f car-seconds on the ribbon (%.1f over collision holes); "
                "progress",
                contact_count_, resets_, far_steps_ * drive::Vehicle::kStep, hole_steps_ * drive::Vehicle::kStep);
  std::string s = b;
  for (size_t k = 0; k <= cars_.size(); ++k) s += " " + std::to_string(int(field_->progress(k)));
  return s;
}

}  // namespace carview
