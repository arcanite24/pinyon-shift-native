#include "race_view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>

#include "drive/from_physics.h"
#include "garage/garage.h"
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

// SPEC-RACE-AI, AI car upgrades: the faked PI each AI aims at, with P the
// player's and C the car's shown points and the class lo..hi: P pulled 5
// inside the class plus the offset r, else C + 300 when P is further above.
int faked_target(int P, int C, int lo, int hi, int r) {
  if (P > C + 300) return C + 300;
  int t = P;
  if (hi - 5 <= t + 5) t = hi - 5;
  if (t - 5 <= lo + 5) t = lo + 5;
  return std::clamp(t + r, lo, hi);
}

// The +5 flag (SPEC-RACE-AI; what sets it is open). Interim: the one
// flagged car of each measured grid (EVENTS Grid).
bool flagged_participant(const std::string& event, int64_t car) {
  return (event == "FR06" && car == 1419) || (event == "HEADLINE_EVENT" && car == 1171);
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
  {
    std::string ne;  // the names are optional: the media names stand in
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    names_.load(root, state ? state : "", &ne);
  }
  stage_ = event.rfind("stage:", 0) == 0;
  if (stage_) {
    // A Rally stage: no event row is read (the stages are in v4's
    // database); the route and the AI line come from the world's folder.
    event_ = {};
    event_.horizon_id = event;
    event_.route_id = std::atoi(event.c_str() + 6);
    event_.player_grid_position = 1;  // slot 00 (SPEC-EVENTS' rule)
    std::string world = o.stage_world.filename().string();
    if (world.empty()) world = o.stage_world.parent_path().filename().string();
    for (char& ch : world) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    char route_name[40], line_name[40];
    std::snprintf(route_name, sizeof route_name, "TrackRoute%03d.xml", event_.route_id);
    std::snprintf(line_name, sizeof line_name, "route_%03d.owt", event_.route_id);
    const auto media = o.stage_world.parent_path().parent_path();
    if (!race::load_route_file(o.stage_world / "Ribbon_00" / route_name, event_.route_id, route_, &e) ||
        !race::load_owt_file(media / "aiopenworld" / world / "Ribbon_00" / line_name, owt_, &e)) {
      error = e;
      return false;
    }
  } else if (!race::load_event(root, event, o.difficulty, event_, &e) ||
             !race::load_route(root, event_.route_id, route_, &e)) {
    error = e;
    return false;
  }
  // No AI line but checkpoints: a street or nemesis race on a line along
  // the road map (race/street_line.h; ours until Q47).
  street_ = false;
  if (!stage_ && !race::load_owt(root, event_.route_id, owt_, &e)) {
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
  // A Rally stage finishes FinishLine x Length past the start along the AI
  // line (v4's Tracks row; ours: SPEC-V4-CONTENT's interim is progress 1,
  // and the line runs on past the stage), and the field's times are drawn.
  have_stage_track_ = false;
  stage_field_.clear(), split_times_.clear();
  stage_place_ = 0;
  if (stage_) {
    const char* state = std::getenv("SHIFT_STATE_ROOT");
    std::string se;
    have_stage_track_ = state && race::load_stage_track(state, event_.route_id, stage_track_, &se);
    if (!have_stage_track_) {
      std::fprintf(stderr, "race: note: no stage row (%s): the finish at the last waypoint\n",
                   state ? se.c_str() : "SHIFT_STATE_ROOT not set");
    } else {
      // The whole line: route 18's start is 3.5 km along it (a 2 km
      // window put its finish 1.5 km early).
      if (gate) line_.gate_progress = line_.line.nearest(gate->x, gate->z, 0, line_.line.length());
      const double want = line_.gate_progress + stage_track_.finish_line * stage_track_.length;
      const double p = std::min(line_.line.length() - 1, want);
      if (p < want)
        std::fprintf(stderr, "race: note: route %d's line ends %.0f m short of the stage finish (%.0f of %.0f m)\n",
                     event_.route_id, want - p, p - line_.gate_progress, want - line_.gate_progress);
      double x, y, z, h;
      line_.line.pose(p, 0, x, y, z, h);
      finish = race::Gate{x, z, std::sin(h), std::cos(h), 40};
      stage_finish_progress_ = p;
      race::RallyField field;
      if (race::load_rally_field(state, field, &se))
        stage_field_ = race::field_times(field, stage_track_, int(o.difficulty), o.seed);
      std::printf("race: stage track %lld, %.0f m, finish at %.4f, %zu splits, best %.2f s, field of %zu\n",
                  (long long)stage_track_.track_id, stage_track_.length, stage_track_.finish_line,
                  stage_track_.splits.size(), stage_track_.best_total(), stage_field_.size());
    }
  }
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
  // SPEC-EVENTS Grid (PIWithPlayerLast): the AI by faked PI, highest on
  // pole, around the player's slot; ties in row order (interim).
  if (event_.upgrade_ai) {
    std::vector<garage::ClassBand> bands;
    std::string be;
    const double ppi = o.player_pi >= 0 ? o.player_pi : race::stock_performance_index(root, o.player_car);
    const int k = event_.target_class;
    if (ppi > 0 && garage::load_class_bands(root, bands, &be) && k >= 1 && size_t(k) < bands.size()) {
      const int lo = int(bands[size_t(k - 1)].shown_max) + 1, hi = int(bands[size_t(k)].shown_max);
      const int P = garage::shown_index(ppi, bands);
      auto faked = [&](const race::Opponent& w) {
        return faked_target(P, garage::shown_index(w.performance_index, bands), lo, hi,
                            race::faked_pi_offset(w.car_id, flagged_participant(event_.horizon_id, w.car_id)));
      };
      std::stable_sort(event_.opponents.begin(), event_.opponents.end(),
                       [&](const race::Opponent& a, const race::Opponent& b) { return faked(a) > faked(b); });
    }
  }
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
  // SPEC-RACE-AI, AI car upgrades (interim until the PI lap): each AI aims
  // at its faked PI (faked_target); t = gap / 240 up, gap / 170 down,
  // capped at 1.
  if (event_.upgrade_ai) {
    std::vector<garage::ClassBand> bands;
    std::string be;
    double ppi = o.player_pi >= 0 ? o.player_pi : race::stock_performance_index(root, o.player_car);
    const int k = event_.target_class;
    if (ppi > 0 && garage::load_class_bands(root, bands, &be) && k >= 1 && size_t(k) < bands.size()) {
      const int lo = int(bands[size_t(k - 1)].shown_max) + 1, hi = int(bands[size_t(k)].shown_max);
      const int P = garage::shown_index(ppi, bands);
      for (auto& c : cars_) {
        const int C = garage::shown_index(c.who.performance_index, bands);
        const int target = faked_target(
            P, C, lo, hi, race::faked_pi_offset(c.who.car_id, flagged_participant(event_.horizon_id, c.who.car_id)));
        const int gap = target - C;
        c.upgrade_t = gap >= 0 ? std::min(1.0, gap / 240.0) : -std::min(1.0, -gap / 170.0);
        std::printf("race: AI upgrade %-28s %d points, target %d (player %d, class %d to %d): t %+.2f\n",
                    c.who.car_media.c_str(), C, target, P, lo, hi, c.upgrade_t);
      }
    } else {
      std::fprintf(stderr, "race: note: no AI upgrades (player PI %.3f, class %d)\n", ppi, k);
    }
  }
  if (finish) finish_progress_ = line_.line.nearest(finish->x, finish->z, line_.gate_progress, line_.line.length());
  // A stage's finish is placed by progress; the nearest point can be an
  // earlier pass where the line doubles back.
  if (stage_ && have_stage_track_) finish_progress_ = stage_finish_progress_;
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
    // One model per car and paint: the row's ColorSequence picks the colour.
    const std::string key = c.who.car_media + "#" + std::to_string(c.who.color_sequence);
    auto it = models_.find(key);
    if (it == models_.end()) {
      auto m = std::make_unique<Showroom>(renderer);
      m->set_guest();
      ShowroomOptions options;
      if (c.who.color_sequence > 0) options.match_sequence = c.who.color_sequence;
      std::string e;
      if (!m->load(bake_dir_, c.who.car_media, options, e) || !m->upload(e)) {
        error = c.who.car_media + ": " + e;
        return false;
      }
      it = models_.emplace(key, std::move(m)).first;
    }
    c.model = it->second.get();
    std::vector<std::string> notes;
    drive::CarParams params = drive::params_for_baked_car(c.model->car(), bake_dir_, &notes);
    if (event_.upgrade_ai) {
      // SPEC-RACE-AI, AI car upgrades: one fraction t per car moves mass,
      // torque and the four tyre-friction scales together (t from open()).
      const double t = c.upgrade_t;
      const double m = t >= 0 ? 1 - 0.1 * t : 1 - 0.111 * t;
      const double q = t >= 0 ? 1 + 0.4 * t : 1 + 0.286 * t;
      const double f = t >= 0 ? 1 + 0.2 * t : 1 + 0.167 * t;
      params.mass *= m;
      params.torque_scale *= q;
      for (int a = 0; a < 2; ++a) params.long_scale[size_t(a)] *= f, params.lat_scale[size_t(a)] *= f;
    }
    c.body = std::make_unique<drive::Vehicle>(params);
    c.body->set_ai_controls(true);
    if (event_.upgrade_ai) {
      c.capacity = drive::ai_capacity(params);
    } else {
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

void RaceView::clip_room(world::Collision& collision) {
  auto& L = line_.line;
  const size_t n = L.size();
  if (n < 2 || L.room_right.size() != n || L.room_left.size() != n) return;
  constexpr double kChunk = 100, kRadius = 90, kHalfCar = 1.0, kUp = 0.6;
  world::CollisionMesh mesh;
  std::vector<std::array<float, 9>> walls;
  double chunk_at = -1e9;
  size_t clipped = 0;
  double worst = 0;
  for (size_t i = 0; i < n; ++i) {
    const double d = L.dist[i];
    if (d - chunk_at > kChunk) {
      chunk_at = d;
      double cx, cy, cz, ch;
      L.pose(std::min(L.length(), d + 0.5 * kChunk), 0, cx, cy, cz, ch);
      mesh = {};
      std::string e;
      if (!collision.gather(cx, cz, kRadius, mesh, &e)) continue;
      walls.clear();
      for (size_t t = 0; t < mesh.triangles(); ++t) {
        if (!world::Collision::collides(mesh, t)) continue;
        const auto& a = mesh.positions[3 * t], &b = mesh.positions[3 * t + 1], &c = mesh.positions[3 * t + 2];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9 || std::fabs(ny) / len > 0.64) continue;  // floors and ramps
        walls.push_back({a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]});
      }
    }
    double x, y, z, h;
    L.pose(d, 0, x, y, z, h);
    if (L.y.size() == n) y = L.y[i];
    y += kUp;
    // Right of the line: heading 0 faces +z, positive towards +x.
    const double rx = std::cos(h), rz = -std::sin(h);
    for (int side = 0; side < 2; ++side) {
      float& room = side == 0 ? L.room_right[i] : L.room_left[i];
      const double sx = side == 0 ? rx : -rx, sz = side == 0 ? rz : -rz;
      const double reach = room + kHalfCar;
      double best = reach;
      for (const auto& w : walls) {
        // Moller-Trumbore, the ray (x, y, z) + s (sx, 0, sz).
        const double e1x = w[3] - w[0], e1y = w[4] - w[1], e1z = w[5] - w[2];
        const double e2x = w[6] - w[0], e2y = w[7] - w[1], e2z = w[8] - w[2];
        const double px = -sz * e2y, py = sz * e2x - sx * e2z, pz = sx * e2y;
        const double det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) < 1e-9) continue;
        const double tx = x - w[0], ty = y - w[1], tz = z - w[2];
        const double u = (tx * px + ty * py + tz * pz) / det;
        if (u < 0 || u > 1) continue;
        const double qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
        const double v = (sx * qx + sz * qz) / det;
        if (v < 0 || u + v > 1) continue;
        const double s = (e2x * qx + e2y * qy + e2z * qz) / det;
        if (s > 0 && s < best) best = s;
      }
      if (best < reach) {
        const float to = float(std::max(0.3, best - kHalfCar));
        if (to < room) worst = std::max(worst, double(room - to)), room = to, ++clipped;
      }
    }
  }
  std::printf("race: room clipped to a wall at %zu of %zu line sides (up to %.1f m)\n", clipped, 2 * n, worst);
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
  // SHIFT_PLAN_DUMP=<file>: the player's corner pass per line point, once.
  if (const char* dump = std::getenv("SHIFT_PLAN_DUMP"); dump && !plan_dumped_ && !planner_fit_) {
    plan_dumped_ = true;
    if (FILE* f = std::fopen(dump, "w")) {
      const auto& L = line_.line;
      std::fprintf(f, "dist,x,y,z,kappa,kappa_v,bank,grade,corner\n");
      for (size_t j = 0; j < L.size(); ++j)
        std::fprintf(f, "%.1f,%.2f,%.2f,%.2f,%.5f,%.5f,%.4f,%.4f,%.2f\n", L.dist[j], L.x[j], L.y.empty() ? 0.f : L.y[j],
                     L.z[j], L.curvature[j], L.curvature_v[j], L.bank_left[j], L.grade[j], field_->corner_speed(n, j));
      std::fclose(f);
    }
    // And the AI line file's points beside it (<file>.owt.csv).
    if (FILE* f = std::fopen((std::string(dump) + ".owt.csv").c_str(), "w")) {
      std::fprintf(f, "x,y,z,lx,ly,lz,offset\n");
      for (const auto& p : owt_.points)
        std::fprintf(f, "%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f\n", p.x, p.y, p.z, p.lx, p.ly, p.lz, p.offset);
      std::fclose(f);
    }
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
    c.before = c.body->pose();
    c.body->step(in, ribbon ? static_cast<const drive::Ground&>(*ribbon_) : near);
  }
  // Contacts: the player and traffic in group 1, the field in 0; a car
  // just reset touches nothing for 3 s. No bounce, and the field's cars
  // keep a fifth of a hit's spin (RACE_AI, Contacts and resets, Q55:
  // RaceCarTorqueScaleInRace 0.2, the misspelt restitution key leaving
  // it 0; interim: 0.2 against every body).
  constexpr double kRaceCarTorqueScale = 0.2;
  contacts_.clear();
  if (session_->collides(n)) contacts_.push_back({&player, 1.0, 1});
  for (size_t k = 0; k < n; ++k)
    if (session_->collides(k)) contacts_.push_back({cars_[k].body.get(), kRaceCarTorqueScale, 0});
  if (traffic) traffic->contact_bodies(contacts_, 1);
  drive::ContactOptions contact_options;
  contact_options.restitution = 0;
  contact_count_ += drive::resolve_contacts(contacts_, contact_options);
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
  // Wrong way (ours: no player wrong-way rule in the specs): facing back
  // along the line and moving; it builds over a second, clears in a third.
  {
    const auto& q = samples_[n];
    const bool against = session_->started() && !session_->finished(n) && q.heading_cos < -0.35 && q.speed > 3;
    wrong_way_ = against ? std::min(3.0, wrong_way_ + drive::Vehicle::kStep)
                         : std::max(0.0, wrong_way_ - 3 * drive::Vehicle::kStep);
  }
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
                   "\"far\":%d,\"state\":%d,\"torque\":%.3f,\"hit\":%.1f,\"hny\":%.2f,\"surf\":%d}\n",
                   session_->clock(), i, q.x1, q.z1, q.speed, q.progress, field_->cross_track(i), lanes_[i],
                   field_->target_speed(i), inputs_[i].throttle, inputs_[i].brake, inputs_[i].steer, q.off_route,
                   q.heading_cos, v.gear(), i < n ? int(cars_[i].far) : 0, i < n ? int(cars_[i].ai.state()) : -1,
                   v.torque_factor(), v.strongest_body_contact().force / 1000, v.strongest_body_contact().normal.y,
                   v.strongest_body_contact().surface);
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
  // A stage's splits (SplitPoints fractions of the length past the start)
  // and, at the finish, the place among the timed field.
  if (have_stage_track_ && stage_place_ == 0) {
    if (session_->finished(n)) {
      const double t = session_->car(n).finish;
      stage_place_ = race::rally_place(stage_field_, t);
      std::vector<double> sorted = stage_field_;
      std::sort(sorted.begin(), sorted.end());
      std::printf("race: stage time %.3f s: %d of %zu (field %.2f to %.2f s)\n", t, stage_place_,
                  stage_field_.size() + 1, sorted.empty() ? 0.0 : sorted.front(), sorted.empty() ? 0.0 : sorted.back());
    } else if (split_times_.size() < stage_track_.splits.size() && session_->clock() > 0 &&
               field_->progress(n) >=
                   line_.gate_progress + stage_track_.splits[split_times_.size()] * stage_track_.length) {
      split_times_.push_back(session_->clock());
      std::printf("race: t %.2f s: split %zu\n", session_->clock(), split_times_.size());
    }
  }
}

void RaceView::instances(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                         std::vector<Showroom::Instance>& out) const {
  for (const auto& c : cars_)
    if (c.model && c.body) {
      if (blend_ >= 1) {
        out.push_back(guest_instance(player_info, player, *c.model, *c.body));
        continue;
      }
      const drive::Vehicle::Pose now = c.body->pose();
      c.body->set_pose(drive::Vehicle::blend(c.before, now, blend_));
      out.push_back(guest_instance(player_info, player, *c.model, *c.body));
      c.body->set_pose(now);
    }
}

std::string RaceView::name_of(size_t i) const {
  if (i >= cars_.size()) return "player";
  return "AI " + std::to_string(cars_[i].who.ai_player_id) + " (" + cars_[i].who.car_media + ")";
}

RaceHud RaceView::hud_info() const {
  RaceHud h;
  if (!session_) return h;
  const size_t n = cars_.size();
  const auto& s = *session_;
  // The event's name from the string tables; its id without one.
  h.event = event_.name.empty() ? event_.horizon_id : names_.text(event_.name);
  if (h.event.empty() || h.event.rfind("_&", 0) == 0) h.event = event_.horizon_id;
  h.circuit = circuit_;
  h.stage = have_stage_track_;
  h.clock = s.clock();
  h.digit = s.digit();
  h.wrong_way = wrong_way_ > 1.0;
  h.cars = int(n + 1);
  h.laps = s.laps();
  h.lap = s.lap(n);
  const auto& me = s.car(n);
  h.place = me.place;
  h.best_lap = me.best_lap;
  h.last_lap = me.laps.empty() ? -1 : me.laps.back();
  h.lap_time = h.clock;
  for (double t : me.laps) h.lap_time -= t;
  const double run = std::max(1.0, finish_progress_ - line_.gate_progress);
  h.progress = std::clamp((field_->progress(n) - line_.gate_progress) / run, 0.0, 1.0);
  if (s.finished(n)) {
    h.phase = RaceHud::kFinished;
    h.finish_time = me.finish;
    h.credits = award_ ? award_->credits : credits_for(me.place);
    h.wristband = award_ ? award_->wristband_points : 0;
    h.stage_place = stage_place_;
    h.stage_field = int(stage_field_.size()) + 1;
  } else {
    h.phase = h.clock < -3 ? RaceHud::kGrid : h.clock < 0 ? RaceHud::kCountdown : RaceHud::kRacing;
  }
  // The standings (SPEC-UI-LAYOUTS b2): the drivers' names (AIPlayers),
  // and each car's gap to the leader, from the distance along the line
  // over the car's speed (ours: the title's gap rule is not specified).
  double lead = -1;
  for (size_t i : s.order()) {
    RaceHudRow r;
    r.player = i == n;
    if (r.player) {
      r.name = "YOU";
    } else {
      r.name = cars_[i].who.driver.empty() ? std::string() : names_.text(cars_[i].who.driver);
      if (r.name.empty() || r.name.rfind("_&", 0) == 0) r.name = names_.short_name(cars_[i].who.car_media);
    }
    r.place = int(h.rows.size()) + 1;
    r.finished = s.finished(i);
    r.time = s.car(i).finish;
    const double p = field_->progress(i);
    if (lead < 0) lead = p;
    else if (i < samples_.size()) r.gap = std::max(0.0, lead - p) / std::max(5.0, double(samples_[i].speed));
    h.rows.push_back(std::move(r));
  }
  return h;
}

std::string RaceView::banner() const {
  const size_t n = cars_.size();
  const auto& s = *session_;
  char b[160], t[32];
  const double c = s.clock();
  if (c < -3) std::snprintf(b, sizeof b, "%s - %s - %zu cars", event_.horizon_id.c_str(),
                            circuit_ ? (std::to_string(s.laps()) + " laps").c_str() : "sprint", n + 1);
  else if (c < 0) std::snprintf(b, sizeof b, "%d", s.digit());
  else if (s.finished(n) && have_stage_track_) {
    clock_text(s.car(n).finish, t, sizeof t);
    std::snprintf(b, sizeof b, "STAGE TIME %s  %d OF %zu", t, stage_place_, stage_field_.size() + 1);
  } else if (s.finished(n)) {
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
  // A Rally stage pays nothing and writes no race result (SPEC-V4-CONTENT:
  // stages pay 0; championships, XP and their records are later work).
  if (stage_ || award_ || !session_->finished(n)) return nullptr;
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
