#include "race_view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <numbers>
#include <thread>

#include "drive/from_physics.h"
#include "drive/replay.h"
#include "drive/rewind.h"
#include "garage/garage.h"
#include "race/player_pi.h"
#include "race/street_line.h"
#include "race/rally_championship.h"
#include "fh1data/archive.h"
#include "traffic/nav.h"
#include "traffic_view.h"
#include "world/collision.h"
#include "world/vfx.h"

namespace carview {

double resolve_player_race_pi(const garage::Car* selected_garage_car, const std::string& active_car_media,
                              const std::map<int64_t, std::string>& baked_ids, const cars::PartsDb* db,
                              const cars::PhysicsSettings* phys_settings, const garage::PiConfig* pi_cfg,
                              const std::vector<garage::ClassBand>* bands) {
  return race::resolve_player_race_pi(selected_garage_car, active_car_media, baked_ids, db, phys_settings, pi_cfg,
                                      bands);
}

namespace {

bool is_under_tunnel_roof(const drive::Ground* ground, const drive::Vec3& pos) {
  if (!ground) return false;
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;
  drive::Hit hit{};
  if (!ground->raycast({pos.x, pos.y + 1.6, pos.z}, {0.0, 1.0, 0.0}, 35.0, hit)) return false;
  return hit.distance >= 0.5 && std::fabs(hit.normal.y) > 0.40;
}

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
// SPEC-RACE-AI's steering at speed (the slip rule): enabled by default;
// set SHIFT_AI_STEER=pursuit to opt out to pure pursuit.
bool slip_rule() {
  static const bool on = [] {
    const char* e = std::getenv("SHIFT_AI_STEER");
    return !(e && std::string(e) == "pursuit");
  }();
  return on;
}

int faked_target(int P, int C, int lo, int hi, int r) {
  if (P > C + 300) return C + 300;
  int t = P;
  if (hi - 5 <= t + 5) t = hi - 5;
  if (t - 5 <= lo + 5) t = lo + 5;
  return std::clamp(t + r, lo, hi);
}

// O(log N) equivalent of drive::RacingLine::segment(distance, 0) for hot 120 Hz paths.
size_t line_segment_fast(const drive::RacingLine& L, double distance) {
  const size_t n = L.size();
  if (n < 2) return 0;
  const auto it =
      std::upper_bound(L.dist.begin(), L.dist.end(), distance, [](double v, float d) { return v < double(d); });
  if (it == L.dist.begin()) return 0;
  return std::min(size_t((it - L.dist.begin()) - 1), n - 2);
}

void line_heading_and_room_fast(const drive::RacingLine& L, double distance, bool right, double& heading,
                                double& room) {
  heading = 0.0;
  room = 1e9;
  const size_t n = L.size();
  if (n < 2) return;
  const size_t s = line_segment_fast(L, distance);
  heading = std::atan2(double(L.x[s + 1] - L.x[s]), double(L.z[s + 1] - L.z[s]));
  const auto& r = right ? L.room_right : L.room_left;
  if (r.size() == n) room = double(r[s]);
}

// The +5 flag (SPEC-RACE-AI; what sets it is open). Interim: the one
// flagged car of each measured grid (EVENTS Grid).
bool flagged_participant(const std::string& event, int64_t car) {
  return (event == "FR06" && car == 1419) || (event == "HEADLINE_EVENT" && car == 1171);
}

}  // namespace

struct RaceView::PhysicsStepPool {
  std::vector<std::thread> workers;
  std::mutex mu;
  std::condition_variable cv_work;
  std::condition_variable cv_done;
  std::function<void(size_t)> fn;
  size_t count = 0;
  std::atomic<size_t> next{0};
  size_t active = 0;
  uint64_t generation = 0;
  bool stop = false;

  explicit PhysicsStepPool(size_t num_workers) {
    workers.reserve(num_workers);
    for (size_t i = 0; i < num_workers; ++i) {
      workers.emplace_back([this]() {
        uint64_t seen = 0;
        for (;;) {
          {
            std::unique_lock<std::mutex> lock(mu);
            cv_work.wait(lock, [&]() { return stop || generation != seen; });
            if (stop) return;
            seen = generation;
          }
          for (;;) {
            const size_t k = next.fetch_add(1, std::memory_order_relaxed);
            if (k >= count) break;
            fn(k);
          }
          {
            std::lock_guard<std::mutex> lock(mu);
            if (--active == 0) cv_done.notify_one();
          }
        }
      });
    }
  }

  ~PhysicsStepPool() {
    {
      std::lock_guard<std::mutex> lock(mu);
      stop = true;
    }
    cv_work.notify_all();
    for (auto& w : workers)
      if (w.joinable()) w.join();
  }

  void run(size_t n, std::function<void(size_t)> task) {
    if (n <= 1 || workers.empty()) {
      for (size_t k = 0; k < n; ++k) task(k);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu);
      fn = std::move(task);
      count = n;
      next.store(0, std::memory_order_relaxed);
      active = workers.size();
      ++generation;
    }
    cv_work.notify_all();
    // Main thread participates too so zero thread sits idle while waiting.
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= count) break;
      fn(k);
    }
    std::unique_lock<std::mutex> lock(mu);
    cv_done.wait(lock, [&]() { return active == 0; });
    fn = nullptr;
  }
};

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
  std::filesystem::path state_root = o.state_root;
  game_root_ = root;
  if (state_root.empty()) {
    if (const char* env_state = std::getenv("SHIFT_STATE_ROOT")) state_root = env_state;
  }
  {
    std::string ne;  // the names are optional: the media names stand in
    names_.load(root, state_root, &ne, o.language, o.parts_db);
    bands_.clear();
    std::string be;
    if (!garage::load_class_bands(root, bands_, &be)) bands_.clear();
    std::string pe;
    if (!garage::load_pi_config(root / "media" / "physics.zip", pi_cfg_, &pe)) pi_cfg_ = garage::synthetic_pi_config();
    player_pi_ = o.player_pi >= 0 ? o.player_pi : race::stock_performance_index(root, o.player_car, state_root, o.parts_db);
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
    std::filesystem::path rally_pkg = o.rally_package;
    if (rally_pkg.empty() && !state_root.empty()) rally_pkg = race::find_rally_package(state_root);

    bool route_ok = race::load_route_file(o.stage_world / "Ribbon_00" / route_name, event_.route_id, route_, &e);
    if (!route_ok && !rally_pkg.empty()) {
      std::string zwhy;
      if (auto zip = fh1::Archive::open(rally_pkg / "Media" / "DLCZips" / "1600_pri_65.zip", &zwhy)) {
        auto r = zip->read(std::string("media/Tracks/ColoradoDirt/Ribbon_00/") + route_name);
        if (r.ok()) {
          route_.id = event_.route_id;
          route_ok = race::parse_route(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()),
                                       route_, &e);
        }
      }
    }
    bool owt_ok = race::load_owt_file(media / "aiopenworld" / world / "Ribbon_00" / line_name, owt_, &e);
    if (!owt_ok && !rally_pkg.empty()) {
      owt_ok = race::load_owt_file(rally_pkg / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "AIOpenWorld" /
                                       "coloradodirt" / "Ribbon_00" / line_name,
                                   owt_, &e);
    }
    if (!route_ok || !owt_ok) {
      error = e;
      return false;
    }
    if (!o.stage_title.empty()) event_.name = o.stage_title;

    // Load Co-Driver pace notes & RallyCall gates for this stage.
    codriver_.reset();
    race::CoDriverAudioMap cd_map;
    std::vector<race::RallyCallGate> gates;
    if (!rally_pkg.empty()) race::load_codriver_audio_map(rally_pkg, cd_map, nullptr);
    if (race::load_rally_calls(o.stage_world, rally_pkg, event_.route_id, gates, nullptr) && !gates.empty()) {
      codriver_.load(std::move(gates), cd_map);
      std::printf("race: loaded %zu Co-Driver RallyCall gates for route %d\n", codriver_.gate_count(), event_.route_id);
    }
  } else if (!race::load_event(root, event, o.difficulty, event_, &e, state_root, o.parts_db, o.ai_mode) ||
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
    std::string se;
    have_stage_track_ =
        !state_root.empty() && race::load_stage_track(state_root, event_.route_id, stage_track_, &se, o.parts_db);
    if (!have_stage_track_) {
      std::fprintf(stderr, "race: note: no stage row (%s): the finish at the last waypoint\n",
                   !state_root.empty() ? se.c_str() : "SHIFT_STATE_ROOT not set");
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
      if (race::load_rally_field(state_root, field, &se))
        stage_field_ = race::field_times(field, stage_track_, int(o.difficulty), o.seed);
      std::printf("race: stage track %lld, %.0f m, finish at %.4f, %zu splits, best %.2f s, field of %zu\n",
                  (long long)stage_track_.track_id, stage_track_.length, stage_track_.finish_line,
                  stage_track_.splits.size(), stage_track_.best_total(), stage_field_.size());
    }
  }
  const auto slots = route_.slots();
  const bool is_showcase_event = !stage_ && race::is_airborne_showcase(event_.career_type, event_.horizon_id);
  const auto cps = race::checkpoints(route_);
  checkpoint_gates_.clear();
  checkpoint_progress_.clear();
  if (street_ && !slots.empty()) {
    // Keep an explicit sprint finish gantry (e.g. Route 206 / PLANE_RACE_006) when present;
    // street races without end_race_cannon_* finish at the last checkpoint's plane.
    if (!finish && !cps.empty()) finish = race::checkpoint_gate(route_, cps.size() - 1);
    if (!gate) {
      const auto* s0 = slots[0];
      gate = race::Gate{s0->x, s0->z, s0->fx, s0->fz, 60};
      line_.gate_progress = line_.line.nearest(s0->x, s0->z, 0, std::min(400.0, line_.line.length())) + 5;
    }
  }
  // Load route_checkpoint_NN gates for street races AND all airborne showcase events
  // (even when a showcase has a .owt AI line and street_ == false).
  if ((street_ || is_showcase_event) && !cps.empty() && !slots.empty()) {
    if (!gate) {
      const auto* s0 = slots[0];
      gate = race::Gate{s0->x, s0->z, s0->fx, s0->fz, 60};
      line_.gate_progress = line_.line.nearest(s0->x, s0->z, 0, std::min(400.0, line_.line.length())) + 5;
    }
    if (!finish) finish = race::checkpoint_gate(route_, cps.size() - 1);
    double from = line_.gate_progress;
    for (size_t k = 0; k < cps.size(); ++k) {
      if (auto g = race::checkpoint_gate(route_, k)) {
        checkpoint_gates_.push_back(*g);
        const double win_end = std::min(line_.line.length(), from + 4000.0);
        from = line_.line.nearest(g->x, g->z, from, win_end);
        checkpoint_progress_.push_back(from);
      }
    }
    // When the route has an explicit finish line past the final intermediate checkpoint
    // (e.g. Route 206 / PLANE_RACE_006), append it so every checkpoint must be cleared first.
    if (finish && !checkpoint_gates_.empty()) {
      const auto& last_cp = checkpoint_gates_.back();
      if (std::hypot(finish->x - last_cp.x, finish->z - last_cp.z) > 15.0) {
        checkpoint_gates_.push_back(*finish);
        const double fin_p = line_.line.nearest(finish->x, finish->z, from, line_.line.length());
        checkpoint_progress_.push_back(fin_p);
      }
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
    const double ppi = o.player_pi >= 0 ? o.player_pi : race::stock_performance_index(root, o.player_car, state_root, o.parts_db);
    const int k = event_.target_class;
    if (ppi > 0 && garage::load_class_bands(root, bands, &be) && k >= 1 && size_t(k) < bands.size()) {
      const int lo = int(bands[size_t(k - 1)].shown_max) + 1, hi = int(bands[size_t(k)].shown_max);
      const int P = garage::shown_index(ppi, bands);
      auto faked = [&](const race::Opponent& w) {
        return faked_target(P, garage::shown_index(w.performance_index, bands), lo, hi,
                            race::faked_pi_offset(w.car_id, flagged_participant(event_.horizon_id, w.car_id)));
      };
      std::stable_sort(event_.opponents.begin(), event_.opponents.end(),
                       [&](const race::Opponent& a, const race::Opponent& b) {
                         const int fa = faked(a), fb = faked(b);
                         if (fa != fb) return fa > fb;
                         return a.performance_index > b.performance_index;
                       });
    }
  } else {
    // Stock-class grid sort: highest PI on pole even when UpgradeAI is false.
    std::stable_sort(event_.opponents.begin(), event_.opponents.end(),
                     [](const race::Opponent& a, const race::Opponent& b) {
                       return a.performance_index > b.performance_index;
                     });
  }
  const int cars = std::min(int(slots.size()), int(event_.opponents.size()) + 1);
  player_slot_ = race::player_slot(event_.player_grid_position, cars);
  const auto opp = race::opponent_slots(player_slot_, cars - 1);
  race::Session::Options sopt;
  sopt.player_index = size_t(cars - 1);  // start-line Hand-of-God launch grace (< 25 s && !has_launched)
  {
    // GameTunableSettings.ini [HandOfGodReset]: the reset's fades, its
    // collision windows and the respawn speed (OptimalSpeedScale).
    std::string he;
    hand_of_god_ = {};
    if (!race::load_hand_of_god_reset(root, hand_of_god_, &he))
      std::fprintf(stderr, "race: note: [HandOfGodReset] not read (%s): its values as fallbacks\n", he.c_str());
    for (const auto& k : hand_of_god_.missing) std::fprintf(stderr, "race: note: no %s\n", k.c_str());
    race::apply_hand_of_god(hand_of_god_, sopt);
    // WORLD-04: the AI resets by its own ResetParameters (AIRacing.xml, the
    // race's settings set), the player alone by the Hand of God triggers.
    // Without the file the AI does not reset automatically.
    const char* set = race::ai_settings_set(street_ || race::event_uses_traffic(event_.career_type));
    race::AiResetRules rules;
    std::string ae;
    if (race::load_ai_reset_rules(root, set, rules, &ae)) sopt.ai_rules = rules;
    else std::fprintf(stderr, "race: note: no AI reset rules (%s): AI cars do not reset\n", ae.c_str());
    sopt.ai_hand_of_god = false;
  }
  if (street_ || race::event_uses_traffic(event_.career_type)) {
    // Street & Nemesis open roads have wider shoulders/intersections than closed festival ribbons.
    sopt.off_route_near = 12.0;
    sopt.off_route_far = 25.0;
  }
  session_ = std::make_unique<race::Session>(size_t(cars), circuit_, laps, *gate, finish, sopt);
  // Upgraded closed-road festival packs run wider side-by-side (0.04); street
  // routes and stock showcase cars stay tighter to the line (0.08).
  const float off_slow = (event_.upgrade_ai && !street_) ? 0.04f : 0.08f;
  field_ = std::make_unique<drive::AiField>(line_.line, size_t(cars),
                                            drive::AiField::Options{.full_gas = true, .off_line_slow = off_slow, .slip_rule = slip_rule()});
  const double from = circuit_ ? line_.gate_progress - 0.5 * line_.lap_length : std::max(0.0, line_.gate_progress - 1500);
  cars_.clear();
  cars_.resize(size_t(cars - 1));
  const bool pinyon_hard = race::is_pinyon_hard(o.difficulty, o.ai_mode);
  const bool effective_rb = o.rubberband && o.ai_mode == race::AiMode::kStock && !pinyon_hard;
  for (size_t k = 0; k < cars_.size(); ++k) {
    Car& c = cars_[k];
    c.who = event_.opponents[k];
    c.slot = opp[k];
    c.lamps.rng_state = uint32_t(0x7A31C95Du ^ (uint32_t(k + 1) * 2654435761u));
    c.upgraded_raw_pi = c.who.performance_index;
    c.setup = race::assign_driver_for_mode(event_.event_skill, event_.event_temperament, event_.event_rubberband,
                                           c.who.skill_modifier, c.who.aggro_modifier, c.who.rubberband_modifier,
                                           o.difficulty, o.ai_mode, event_);
    const race::SkillRow* sk = event_.skill(c.setup.skill);
    c.ai.init(sk ? *sk : race::SkillRow{}, event_.rubberband(c.setup.rubberband_id), o.seed * 1000003u + k,
              effective_rb);
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
  // SPEC-RACE-AI, AI car upgrades: compute each AI's faked target PI (solved via
  // 10-iteration bisection over the PI.xml virtual lap once CarParams load in load_cars()).
  if (event_.upgrade_ai) {
    std::vector<garage::ClassBand> bands = bands_;
    std::string be;
    if (bands.empty()) garage::load_class_bands(root, bands, &be);
    double ppi = o.player_pi >= 0 ? o.player_pi : race::stock_performance_index(root, o.player_car, state_root, o.parts_db);
    const int k = event_.target_class;
    if (ppi > 0 && k >= 1 && size_t(k) < bands.size()) {
      const int lo = int(bands[size_t(k - 1)].shown_max) + 1, hi = int(bands[size_t(k)].shown_max);
      const int P = garage::shown_index(ppi, bands);
      for (auto& c : cars_) {
        const int C = garage::shown_index(c.who.performance_index, bands);
        const int target = faked_target(
            P, C, lo, hi, race::faked_pi_offset(c.who.car_id, flagged_participant(event_.horizon_id, c.who.car_id)));
        c.target_shown_pi = target;
        c.upgraded_shown_pi = target;
        const int gap = target - C;
        c.upgrade_t = gap >= 0 ? std::min(1.0, gap / 240.0) : -std::min(1.0, -gap / 170.0);
        std::printf("race: AI upgrade %-28s %d points, target %d (player %d, class %d to %d): init t %+.2f\n",
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
  // Airborne Showcases (PLANE_RACE_001..007, CareerTypeId = 8): load airborne_challenges.xml
  // and initialize the animated retail aircraft (P-51 Mustang, Helicopter, Biplane, Hot-Air Balloon).
  showcase_xml_specs_.clear();
  showcase_rumble_ = 0.0;
  if (race::is_airborne_showcase(event_.career_type, event_.horizon_id)) {
    std::string sc_err;
    (void)race::load_airborne_challenges(root, showcase_xml_specs_, &sc_err);
    const race::ShowcaseSpec spec =
        race::resolve_showcase_spec(event_.event_id, event_.horizon_id, showcase_xml_specs_, int(o.difficulty));
    const double fin_p = finish_progress_ > line_.gate_progress + 100.0 ? finish_progress_ : line_.line.length();
    showcase_.init(spec, line_.line, line_.gate_progress, fin_p);
    std::printf("race: airborne showcase opponent %s (event %lld, xml=%s, %zu timing cps, route %.0f m)\n",
                showcase_.spec().display_name.c_str(), (long long)event_.event_id,
                showcase_xml_specs_.empty() ? "defaults" : "airborne_challenges.xml",
                showcase_.spec().timing_checkpoints.size(), fin_p - line_.gate_progress);
  }
  const size_t n = size_t(cars);
  states_.resize(n), inputs_.resize(n), progress_.resize(n), cross_.resize(n), speed_.resize(n), lanes_.assign(n, 0), caps_.assign(n, 1e9f);
  samples_.resize(n);
  lane_params_.resize(n);
  last_planner_c_.assign(n, -1.0f);
  last_planner_b_.assign(n, -1.0f);
  cached_event_name_ = event_.name.empty() ? event_.horizon_id : names_.text(event_.name);
  if (cached_event_name_.empty() || cached_event_name_.rfind("_&", 0) == 0) cached_event_name_ = event_.horizon_id;
  cached_player_car_name_ = names_.short_name(o_.player_car);
  for (size_t k = 0; k < cars_.size(); ++k) {
    if (const auto* t = event_.temperament(cars_[k].setup.temperament))
      lane_params_[k] = {float(t->trailing_distance), float(t->car_clearance), float(t->edge_clearance),
                         float(t->aggression)};
    cars_[k].cached_car_name = names_.short_name(cars_[k].who.car_media);
    cars_[k].cached_driver_name = cars_[k].who.driver.empty() ? std::string() : names_.text(cars_[k].who.driver);
    if (cars_[k].cached_driver_name.empty() || cars_[k].cached_driver_name.rfind("_&", 0) == 0)
      cars_[k].cached_driver_name = cars_[k].cached_car_name;
  }
  recorded_ghost_.clear();
  recorded_ghost_.event_id = event_.horizon_id;
  recorded_ghost_.car_media = o.player_car;
  if (player_pi_ > 0.0 && !bands_.empty()) recorded_ghost_.shown_pi = garage::shown_index(player_pi_, bands_);
  if (session_ && !bands_.empty()) {
    for (size_t k = 0; k < cars_.size(); ++k) {
      if (cars_[k].who.performance_index > 0.0) {
        session_->set_car_class(k, garage::class_of(cars_[k].who.performance_index, bands_));
      }
    }
    if (player_pi_ > 0.0) {
      session_->set_car_class(player_index(), garage::class_of(player_pi_, bands_));
    }
  }
  smashables_reset_pending_ = true;
  missed_cp_.reset();
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

void RaceView::route_near(std::vector<std::array<double, 2>>& out) const {
  out.clear();
  if (!field_) return;
  const auto& L = line_.line;
  if (L.dist.empty()) return;
  const double p = field_->progress(cars_.size());
  const double from = p - 150.0;
  const double to = std::min(p + 600.0, finish_progress_ > 0 ? finish_progress_ : L.length());
  auto it = std::lower_bound(L.dist.begin(), L.dist.end(), from, [](float d, double v) { return double(d) < v; });
  for (size_t i = size_t(it - L.dist.begin()); i < L.size() && double(L.dist[i]) <= to; ++i)
    out.push_back({L.x[i], L.z[i]});
}

void RaceView::opponent_positions(std::vector<std::array<double, 2>>& out) const {
  out.clear();
  if (rivals_solo_) return;
  for (const auto& c : cars_)
    if (c.body) out.push_back({c.body->position().x, c.body->position().z});
}

bool RaceView::next_checkpoint_pos(std::array<double, 2>& out) const {
  if (checkpoint_gates_.size() <= 1 || !session_ || session_->finished(player_index())) return false;
  const size_t pi = player_index();
  const size_t k = pi < next_checkpoint_.size() ? next_checkpoint_[pi] : 0;
  const size_t last = checkpoint_gates_.size() - 1;
  if (k >= last) return false;
  out = {checkpoint_gates_[k].x, checkpoint_gates_[k].z};
  return true;
}

bool RaceView::finish_pos(std::array<double, 2>& out) const {
  if (!checkpoint_gates_.empty()) {
    const auto& fin = checkpoint_gates_.back();
    out = {fin.x, fin.z};
    return true;
  }
  if (line_.line.size() >= 2 && finish_progress_ > 0.0) {
    double x = 0.0, y = 0.0, z = 0.0, h = 0.0;
    line_.line.pose(finish_progress_, 0.0, x, y, z, h);
    out = {x, z};
    return true;
  }
  return false;
}

bool RaceView::leader_opponent_pos(std::array<double, 2>& out) const {
  if (!session_ || rivals_solo_ || cars_.empty()) return false;
  const auto ord = session_->order();
  if (ord.empty()) return false;
  const size_t lead_idx = ord.front();
  if (lead_idx >= cars_.size() || !cars_[lead_idx].body) return false;
  const drive::Vec3 p = cars_[lead_idx].body->position();
  out = {p.x, p.z};
  return true;
}

bool RaceView::post_race_spawn(double out[4]) const {
  const race::RouteTransform* t = route_.find("post_race_location_00");
  if (!t) return false;
  out[0] = t->x, out[1] = t->y, out[2] = t->z, out[3] = std::atan2(t->fx, t->fz);
  return true;
}

bool RaceView::load_cars(Renderer& renderer, const drive::Ground& ground, std::string& error) {
  // Identify unique opponent (car_media, effective color_sequence) pairs and CPU-load them in parallel.
  auto effective_color_seq = [&](const race::Opponent& who) -> int64_t {
    if (who.color_sequence > 0) return who.color_sequence;
    if (const auto* col = event_.ai_player_color(who); col && col->color_sequence > 0) return col->color_sequence;
    return 0;
  };
  struct ModelSpec {
    std::string key;
    std::string media;
    int64_t color_seq = 0;
  };
  // Allocate a dedicated guest Showroom per opponent slot so even single-make races give
  // every opponent independent wheel steer, spin, omega blur, and suspension travel.
  std::vector<ModelSpec> specs;
  for (size_t k = 0; k < cars_.size(); ++k) {
    const auto& c = cars_[k];
    const int64_t cseq = effective_color_seq(c.who);
    const std::string key = c.who.car_media + "#" + std::to_string(cseq) + "@opp" + std::to_string(k);
    if (models_.count(key)) continue;
    specs.push_back({key, c.who.car_media, cseq});
  }
  if (!specs.empty()) {
    struct LoadedSpec {
      std::unique_ptr<Showroom> m;
      std::string err;
    };
    std::vector<LoadedSpec> loaded(specs.size());
    const size_t threads = std::min<size_t>(specs.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
    auto load_one = [&](size_t i) {
      auto m = std::make_unique<Showroom>(renderer);
      m->set_guest();
      ShowroomOptions options;
      if (specs[i].color_seq > 0) options.match_sequence = specs[i].color_seq;
      std::string e;
      if (!m->load(bake_dir_, specs[i].media, options, e)) {
        loaded[i].err = specs[i].media + ": " + e;
        return;
      }
      m->load_car_lamps(game_root_, specs[i].media);
      loaded[i].m = std::move(m);
    };
    if (threads > 1 && specs.size() >= 2) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= specs.size()) break;
            load_one(i);
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      for (size_t i = 0; i < specs.size(); ++i) load_one(i);
    }
    for (size_t i = 0; i < specs.size(); ++i) {
      if (!loaded[i].err.empty() || !loaded[i].m) {
        error = loaded[i].err;
        return false;
      }
      std::string e;
      if (!loaded[i].m->upload(e)) {
        error = specs[i].media + ": " + e;
        return false;
      }
      models_.emplace(specs[i].key, std::move(loaded[i].m));
    }
  }
  auto srgb_to_lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
  for (size_t k = 0; k < cars_.size(); ++k) {
    auto& c = cars_[k];
    const int64_t cseq = effective_color_seq(c.who);
    const std::string key = c.who.car_media + "#" + std::to_string(cseq) + "@opp" + std::to_string(k);
    c.model = models_[key].get();
    c.has_ai_paint = false;
    if (c.who.color_sequence <= 0) {
      if (const auto* col = event_.ai_player_color(c.who); col && col->has_rgb && col->color_sequence <= 0) {
        c.ai_paint_rgb[0] = srgb_to_lin(float((col->rgb >> 16) & 0xFFu) / 255.0f);
        c.ai_paint_rgb[1] = srgb_to_lin(float((col->rgb >> 8) & 0xFFu) / 255.0f);
        c.ai_paint_rgb[2] = srgb_to_lin(float(col->rgb & 0xFFu) / 255.0f);
        c.has_ai_paint = true;
      }
    }
  }
  // Parallelize per-car physics parameter derivation, 10-iteration PI virtual-lap bisection,
  // ai_capacity() table computation, and grid placement across worker threads.
  std::vector<std::string> bisect_logs(cars_.size());
  auto setup_one_car = [&](size_t idx) {
    Car& c = cars_[idx];
    std::vector<std::string> notes;
    drive::CarParams params = drive::params_for_baked_car(c.model->car(), bake_dir_, &notes);
    if (event_.upgrade_ai) {
      // SPEC-RACE-AI, AI car upgrades: retail scaling (1+0.20t / 1-0.167|t| tyre friction,
      // 1+0.4t / 1-0.286|t| torque, 1-0.1t / 1+0.111|t| mass) solved via 10-iteration
      // bisection against the PI.xml virtual lap, plus EventParticipants.TuningLevel polish.
      if (c.target_shown_pi > 0 && !bands_.empty() && c.who.performance_index > 0.0) {
        const double stock_raw = c.who.performance_index;
        const int stock_shown = garage::shown_index(stock_raw, bands_);
        const double stock_lap = garage::simulate_pi_lap(params, nullptr, pi_cfg_);
        const double stock_lap_pi = garage::lap_time_to_pi(stock_lap, pi_cfg_);
        auto raw_pi_for = [&](const drive::CarParams& cand) -> double {
          const double lap = garage::simulate_pi_lap(cand, nullptr, pi_cfg_);
          const double lap_pi = garage::lap_time_to_pi(lap, pi_cfg_);
          return std::clamp(stock_raw + (lap_pi - stock_lap_pi), 0.001, 1.0);
        };
        auto shown_pi_for = [&](const drive::CarParams& cand) -> double {
          return double(garage::shown_index(raw_pi_for(cand), bands_));
        };
        const race::AiUpgradeScales solved =
            race::solve_ai_upgrade(params, double(stock_shown), double(c.target_shown_pi), shown_pi_for, 10);
        c.upgrade_t = solved.t;
        race::apply_ai_upgrade(params, c.upgrade_t, c.who.tuning_level);
        c.upgraded_raw_pi = raw_pi_for(params);
        c.upgraded_shown_pi = garage::shown_index(c.upgraded_raw_pi, bands_);
        char buf[192];
        std::snprintf(buf, sizeof buf, "race: AI bisect %-28s stock %d -> target %d, solved %d (t %+.3f, tune %d)\n",
                      c.who.car_media.c_str(), stock_shown, c.target_shown_pi, c.upgraded_shown_pi, c.upgrade_t,
                      c.who.tuning_level);
        bisect_logs[idx] = buf;
      } else {
        race::apply_ai_upgrade(params, c.upgrade_t, c.who.tuning_level);
      }
    } else if (c.who.tuning_level > 0) {
      race::apply_ai_upgrade(params, 0.0, c.who.tuning_level);
    }
    c.body = std::make_unique<drive::Vehicle>(params);
    c.body->set_ai_controls(true);
    if (event_.upgrade_ai) {
      c.capacity = drive::ai_capacity(params);
    } else {
      // Non-UpgradeAI events (e.g. EXHIBITION_FEST_003 stock Mini 1566) run at
      // exact stock CurbWeight (SPEC-RACE-AI, Contacts and resets).
      c.capacity = drive::ai_capacity(c.body->params());
    }
    c.body->set_ai_dirt_grip(true);
    const auto& s = slots_[size_t(c.slot)];
    double y = s.y;
    double gy = y;
    if (world::ground_height(ground, s.x, s.z, y + 5, gy)) y = gy;
    c.body->reset({s.x, y + 0.05, s.z}, std::atan2(s.fx, s.fz));
    c.px = s.x, c.pz = s.z;
    // Initialize persistent AI lamps on the grid so headlamps match tod_sec immediately.
    drive::LampInputs lin;
    const int vg = c.body->gear();
    lin.gear = (vg == -1) ? 0 : std::max(1, vg);
    lin.tod_sec = double(tod_sec_);
    lin.dt = float(drive::Vehicle::kStep);
    lin.is_ai = true;
    lin.snap_headlamps = true;
    c.lamps.update(lin);
    c.lamps_initialized = true;
  };
  const size_t setup_threads =
      std::min<size_t>(cars_.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
  if (setup_threads > 1 && cars_.size() >= 2) {
    std::atomic<size_t> next_car{0};
    std::vector<std::thread> workers;
    workers.reserve(setup_threads);
    for (size_t t = 0; t < setup_threads; ++t) {
      workers.emplace_back([&]() {
        for (;;) {
          const size_t idx = next_car.fetch_add(1, std::memory_order_relaxed);
          if (idx >= cars_.size()) break;
          setup_one_car(idx);
        }
      });
    }
    for (auto& w : workers) w.join();
  } else {
    for (size_t idx = 0; idx < cars_.size(); ++idx) setup_one_car(idx);
  }
  for (const auto& line_log : bisect_logs) {
    if (!line_log.empty()) std::fputs(line_log.c_str(), stdout);
  }
  if (session_ && !bands_.empty()) {
    for (size_t k = 0; k < cars_.size(); ++k) {
      const double eff_pi = cars_[k].upgraded_raw_pi > 0.0 ? cars_[k].upgraded_raw_pi : cars_[k].who.performance_index;
      if (eff_pi > 0.0) session_->set_car_class(k, garage::class_of(eff_pi, bands_));
    }
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
  if (showcase_.active()) {
    const double probe_top_y = showcase_.pose().position[1] + 400.0;
    constexpr double kProbeSpan = 750.0;
    auto init_probe = [&](double px, double pz) -> std::optional<double> {
      double best_y = -1e30;
      bool found = false;
      drive::Hit hit{};
      if (ground.raycast({px, probe_top_y, pz}, {0.0, -1.0, 0.0}, kProbeSpan, hit)) {
        const double hy = probe_top_y - hit.distance;
        if (std::isfinite(hy)) {
          best_y = hy;
          found = true;
        }
      }
      if (ribbon_) {
        drive::Hit rhit{};
        if (ribbon_->raycast({px, probe_top_y, pz}, {0.0, -1.0, 0.0}, kProbeSpan, rhit)) {
          const double ry = probe_top_y - rhit.distance;
          if (std::isfinite(ry) && (!found || ry > best_y)) {
            best_y = ry;
            found = true;
          }
        }
      }
      return found ? std::optional<double>(best_y) : std::nullopt;
    };
    showcase_.apply_initial_clearance(init_probe);
  }
  return true;
}

void RaceView::clip_room(world::Collision& collision) {
  auto& L = line_.line;
  const size_t n = L.size();
  if (n < 2 || L.room_right.size() != n || L.room_left.size() != n) return;
  constexpr double kChunk = 100, kRadius = 90, kHalfCar = 1.0, kUp = 0.6;
  struct WallTri {
    float v[9];
    float min_x, max_x, min_y, max_y, min_z, max_z;
  };
  world::CollisionMesh mesh;
  std::vector<WallTri> walls;
  double chunk_at = -1e9;
  size_t clipped = 0;
  double worst = 0;
  for (size_t i = 0; i < n; ++i) {
    const double d = L.dist[i];
    if (d - chunk_at > kChunk) {
      chunk_at = d;
      const size_t cs = line_segment_fast(L, std::min(L.length(), d + 0.5 * kChunk));
      const double cf = std::clamp(
          (std::min(L.length(), d + 0.5 * kChunk) - double(L.dist[cs])) /
              std::max(1e-6, double(L.dist[cs + 1] - L.dist[cs])),
          0.0, 1.0);
      const double cx = double(L.x[cs]) + cf * double(L.x[cs + 1] - L.x[cs]);
      const double cz = double(L.z[cs]) + cf * double(L.z[cs + 1] - L.z[cs]);
      mesh = {};
      std::string e;
      if (!collision.gather(cx, cz, kRadius, mesh, &e)) continue;
      walls.clear();
      walls.reserve(mesh.triangles() / 4 + 16);
      for (size_t t = 0; t < mesh.triangles(); ++t) {
        if (!world::Collision::collides(mesh, t)) continue;
        const auto& a = mesh.positions[3 * t], &b = mesh.positions[3 * t + 1], &c = mesh.positions[3 * t + 2];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-9 || std::fabs(ny) / len > 0.64) continue;  // floors and ramps
        WallTri wt;
        wt.v[0] = a[0], wt.v[1] = a[1], wt.v[2] = a[2];
        wt.v[3] = b[0], wt.v[4] = b[1], wt.v[5] = b[2];
        wt.v[6] = c[0], wt.v[7] = c[1], wt.v[8] = c[2];
        wt.min_x = std::min({a[0], b[0], c[0]});
        wt.max_x = std::max({a[0], b[0], c[0]});
        wt.min_y = std::min({a[1], b[1], c[1]});
        wt.max_y = std::max({a[1], b[1], c[1]});
        wt.min_z = std::min({a[2], b[2], c[2]});
        wt.max_z = std::max({a[2], b[2], c[2]});
        walls.push_back(wt);
      }
    }
    const size_t seg = std::min(i, n - 2);
    const double x = double(L.x[i]);
    const double z = double(L.z[i]);
    const double h = std::atan2(double(L.x[seg + 1] - L.x[seg]), double(L.z[seg + 1] - L.z[seg]));
    const double y = (L.y.size() == n ? double(L.y[i]) : 0.0) + kUp;
    // Right of the line: heading 0 faces +z, positive towards +x.
    const double rx = std::cos(h), rz = -std::sin(h);
    const float fy = float(y);
    for (int side = 0; side < 2; ++side) {
      float& room = side == 0 ? L.room_right[i] : L.room_left[i];
      const double sx = side == 0 ? rx : -rx, sz = side == 0 ? rz : -rz;
      const double reach = room + kHalfCar;
      const float rmin_x = float(std::min(x, x + sx * reach)) - 0.05f;
      const float rmax_x = float(std::max(x, x + sx * reach)) + 0.05f;
      const float rmin_z = float(std::min(z, z + sz * reach)) - 0.05f;
      const float rmax_z = float(std::max(z, z + sz * reach)) + 0.05f;
      double best = reach;
      for (const auto& wt : walls) {
        if (fy < wt.min_y || fy > wt.max_y || wt.max_x < rmin_x || wt.min_x > rmax_x || wt.max_z < rmin_z ||
            wt.min_z > rmax_z)
          continue;
        const float* w = wt.v;
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

drive::Input RaceView::player_input(const drive::Vehicle& player, const drive::Input& user,
                                    const TrafficView* traffic) {
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
  const bool is_nemesis = event_.career_type == 11 || event_.style == 5;
  const double player_prog = field_->progress(n);
  if (last_planner_c_.size() != n + 1) {
    last_planner_c_.assign(n + 1, -1.0f);
    last_planner_b_.assign(n + 1, -1.0f);
  }
  auto update_car_planner = [&](size_t idx, const drive::AiCapacity* cap, float C, float B) {
    if (std::fabs(C - last_planner_c_[idx]) <= 1e-4f && std::fabs(B - last_planner_b_[idx]) <= 1e-4f) return;
    last_planner_c_[idx] = C;
    last_planner_b_[idx] = B;
    if (planner_fit_) field_->set_limits(idx, float(race::corner_accel(C)), float(race::brake_decel(B)));
    else field_->set_planner(idx, cap, C, B);
  };
  for (size_t k = 0; k < n; ++k) {
    states_[k] = drive::ai_state(*cars_[k].body);
    double C = cars_[k].ai.cornering(), B = cars_[k].ai.braking();
    if (is_nemesis) {
      const double from_slot = field_->progress(k) - cars_[k].start_progress;
      int other_ahead = 0;
      for (size_t j = 0; j < n; ++j)
        if (j != k && field_->progress(j) > field_->progress(k)) ++other_ahead;
      const double dist_to_fin = finish_progress_ > 0 ? std::max(0.0, finish_progress_ - field_->progress(k)) : 1e9;
      const auto nb = race::nemesis_rival_boost(true, from_slot >= 1000.0, int(n) - 1, other_ahead,
                                                player_prog - field_->progress(k), dist_to_fin, B, C);
      B = nb.braking;
      C = nb.cornering;
    }
    // Cap planned cornering below static rollover stability for narrow/tall
    // chassis (e.g. 1965 Mini 1.2 m track) so catch-up (C = 1.25) never flips
    // them onto their roof; deliberate mode-4 over-cornering stays uncapped.
    const bool over_corner = cars_[k].ai.state() == race::DriverAi::State::kRubberband && cars_[k].ai.mode() == 4;
    if (!over_corner && cars_[k].capacity.lateral[8] > 1.0f) {
      const auto& bp = cars_[k].body->params();
      const double half_track = 0.5 * std::fabs(bp.wheel_centres[1].x - bp.wheel_centres[0].x);
      const double com_h = std::max(0.20, bp.centre_of_mass.y);
      if (half_track > 0.3 && (half_track / com_h) < 1.45) {
        const double roll_cap_ms2 = 0.98 * (half_track / com_h) * 9.81;
        const double max_c = std::max(0.81, roll_cap_ms2 / double(cars_[k].capacity.lateral[8]));
        if (C > max_c) C = max_c;
      }
    }
    update_car_planner(k, &cars_[k].capacity, float(C), float(B));
  }
  states_[n] = drive::ai_state(player);
  update_car_planner(n, &player_capacity_, float(player_ai_.cornering()), float(player_ai_.braking()));
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
  // During the grid & pre-start countdown (before launch), all cars are held stationary on the
  // grid and their AI inputs are overridden in after_step(). Skip the full lane-planning and
  // 420 m braking-curve pass until 50 ms before GO so countdown stays at full framerate.
  if (!session_->started() && session_->clock() < -0.05) {
    for (auto& inp : inputs_) inp = drive::Input{};
    return countdown_grid_input(user, o_.autopilot, session_->clock());
  }
  // Lanes from the last step's progress, then the field's pass.
  for (size_t k = 0; k <= n; ++k)
    progress_[k] = field_->progress(k), cross_[k] = field_->cross_track(k), speed_[k] = states_[k].speed;
  traffic_obs_scratch_.clear();
  if (traffic && traffic->cars() && !traffic->cars()->empty()) {
    double min_p = progress_[0], max_p = progress_[0];
    for (size_t k = 1; k <= n; ++k)
      min_p = std::min(min_p, progress_[k]), max_p = std::max(max_p, progress_[k]);
    const double lo_p = std::max(0.0, min_p - 40.0);
    const double hi_p = std::min(line_.line.length(), max_p + 120.0);
    for (const auto& tc : *traffic->cars()) {
      const double tp = line_.line.nearest(tc.position.x, tc.position.z, lo_p, hi_p);
      double lx, ly, lz, lh;
      line_.line.pose(tp, 0, lx, ly, lz, lh);
      const double dx = tc.position.x - lx, dz = tc.position.z - lz;
      const double rx = std::cos(lh), rz = -std::sin(lh);  // right of line
      const double cross = dx * rx + dz * rz;
      const double along_err = dx * std::sin(lh) + dz * std::cos(lh);
      if (std::fabs(along_err) > 12.0) continue;
      double dummy_h = 0.0, room_val = 1e9;
      line_heading_and_room_fast(line_.line, tp, cross > 0, dummy_h, room_val);
      const double room = std::min(12.0, room_val);
      if (std::fabs(cross) > room + 3.0) continue;
      const double dir_dot = std::sin(tc.heading) * std::sin(lh) + std::cos(tc.heading) * std::cos(lh);
      traffic_obs_scratch_.push_back({tp, float(cross), float(tc.speed), dir_dot < 0.0});
    }
  }
  race::plan_lanes(line_.line, progress_, cross_, speed_, lane_params_, n, lanes_, drive::Vehicle::kStep, 2.1, caps_,
                   traffic_obs_scratch_);
  for (size_t k = 0; k < n; ++k) field_->set_lane(k, lanes_[k]), field_->set_speed_cap(k, caps_[k]);
  field_->drive(states_, inputs_);
  // Held on the grid in first until GO while passing player throttle through for launch revving.
  if (!session_->started()) return countdown_grid_input(user, o_.autopilot, session_->clock());
  if (o_.autopilot || session_->finished(n)) return inputs_[n];
  return user;
}

drive::Input RaceView::countdown_grid_input(const drive::Input& user, bool autopilot, double clock_s) {
  const double thr = autopilot ? (clock_s >= -3.09 ? 1.0 : 0.0)
                               : (std::isfinite(user.throttle) ? std::clamp(user.throttle, 0.0, 1.0) : 0.0);
  // Keep brake = 1.0 when revving (thr > 0) and 0.4 when off-throttle so automatic
  // transmission never triggers the > 0.5 reverse-select hold while sitting on the grid.
  drive::Input held{};
  held.steer = 0.0;
  held.throttle = thr;
  held.brake = thr > 0.0 ? 1.0 : 0.4;
  held.handbrake = true;
  held.clutch = 1.0;
  return held;
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
  reset_car_to_progress(v, i, field_->progress(i), near, cx, cz, radius);
}

void RaceView::reset_car_to_progress(drive::Vehicle& v, size_t i, double p, const drive::Ground& near, double cx,
                                     double cz, double radius) {
  double x, y, z, h;
  line_.line.pose(p, 0, x, y, z, h);
  const bool inside = std::hypot(x - cx, z - cz) < radius - 40;
  const drive::Ground& g = inside ? near : static_cast<const drive::Ground&>(*ribbon_);
  if (i < cars_.size()) cars_[i].far = !inside;
  double gy = y;
  if (world::ground_height(g, x, z, y + 5, gy)) y = gy;
  const double speed = hand_of_god_.optimal_speed_scale * field_->planned_speed(i, p);  // OptimalSpeedScale
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
  const bool is_nemesis = event_.career_type == 11 || event_.style == 5;
  const double player_prog = field_->progress(n);
  // SPEC-CAR-PHYSICS, Aerodynamic drafting: evaluate pairwise follower & leader
  // slipstream multipliers across player, AI opponents, and nearby traffic.
  if (session_->started()) {
    auto to_draft_car = [](const drive::Vehicle& v) {
      drive::DraftCar dc;
      dc.position = v.position();
      dc.forward = drive::rotate(v.orientation(), {0, 0, 1});
      dc.speed = v.speed();
      dc.width = v.params().size.x > 0.5 ? v.params().size.x : 1.9;
      dc.front_df_coeff = v.params().aero_df_front + v.params().aero_part_df_front;
      dc.rear_df_coeff = v.params().aero_df_rear + v.params().aero_part_df_rear;
      return dc;
    };
    auto& pool = draft_pool_scratch_;
    pool.clear();
    pool.reserve(n + 1 + (traffic && traffic->cars() ? traffic->cars()->size() : 0));
    for (size_t k = 0; k < n; ++k) pool.push_back(to_draft_car(*cars_[k].body));
    pool.push_back(to_draft_car(player));
    if (traffic && traffic->cars()) {
      for (const auto& tc : *traffic->cars()) {
        if (tc.speed < 8.0) continue;
        drive::DraftCar dc;
        dc.position = {tc.position.x, tc.position.y, tc.position.z};
        dc.forward = {std::sin(tc.heading), 0.0, std::cos(tc.heading)};
        dc.speed = tc.speed;
        dc.width = 1.9;
        pool.push_back(dc);
      }
    }
    auto& leaders = draft_leaders_scratch_;
    leaders.reserve(pool.size());
    auto& leader_strength = draft_leader_strength_scratch_;
    leader_strength.assign(n + 1, 0.0);
    auto& follower_states = draft_follower_states_scratch_;
    follower_states.resize(n + 1);
    for (size_t i = 0; i <= n; ++i) {
      leaders.clear();
      for (size_t j = 0; j < pool.size(); ++j) {
        if (j == i) continue;
        leaders.push_back(pool[j]);
        if (j <= n) {
          const double d_pair = drive::compute_follower_draft(pool[i], &pool[j], 1).strength;
          if (d_pair > leader_strength[j]) leader_strength[j] = d_pair;
        }
      }
      follower_states[i] = drive::compute_follower_draft(pool[i], leaders.data(), leaders.size());
    }
    auto combine_draft = [](const drive::DraftState& f, const drive::DraftState& l) {
      drive::DraftState out = f;
      out.strength = std::max(f.strength, l.strength);
      out.body_drag = f.body_drag * l.body_drag;
      out.front_drag = f.front_drag * l.front_drag;
      out.rear_drag = f.rear_drag * l.rear_drag;
      out.front_df = f.front_df * l.front_df;
      out.rear_df = f.rear_df * l.rear_df;
      return out;
    };
    for (size_t k = 0; k < n; ++k) {
      const auto ld = drive::compute_leader_draft(leader_strength[k], pool[k].speed);
      cars_[k].body->set_draft(combine_draft(follower_states[k], ld));
    }
    const auto pld = drive::compute_leader_draft(leader_strength[n], pool[n].speed);
    player.set_draft(combine_draft(follower_states[n], pld));
  } else {
    player.set_draft(drive::DraftState{});
    for (size_t k = 0; k < n; ++k) cars_[k].body->set_draft(drive::DraftState{});
  }
  if (n >= 2 && !step_pool_) {
    const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t workers = std::min<size_t>(n - 1, hw > 1 ? hw - 1 : 0);
    if (workers > 0) step_pool_ = std::make_unique<PhysicsStepPool>(workers);
  }
  step_ribbon_scratch_.assign(n, 0);
  step_hole_scratch_.assign(n, 0);
  step_bad_scratch_.assign(n, 0);
  auto& step_ribbon = step_ribbon_scratch_;
  auto& step_hole = step_hole_scratch_;
  auto& step_bad = step_bad_scratch_;
  auto step_ai_car = [&](size_t k) {
    Car& c = cars_[k];
    const drive::Vec3 at = c.body->position();
    c.px = at.x, c.pz = at.z;
    drive::Input in = inputs_[k];
    // Held on the grid; revving from 3.09 s before the signal, clutch dropped at it.
    if (clock < 0) in = clock >= -3.09 ? drive::Input{0, 1, 1, true} : drive::Input{0, 0, 0.4, true};
    else drive::ai_traction(*c.body, c.traction_cap, drive::Vehicle::kStep, in, !slip_rule());
    const double from_slot = field_->progress(k) - c.start_progress;
    const double boost = race::start_boost(c.slot, int(grid_cars), from_slot);
    double nem_torque = 0.0;
    if (is_nemesis && from_slot >= 1000.0) {
      int other_ahead = 0;
      for (size_t j = 0; j < n; ++j)
        if (j != k && field_->progress(j) > field_->progress(k)) ++other_ahead;
      const double dist_to_fin = finish_progress_ > 0 ? std::max(0.0, finish_progress_ - field_->progress(k)) : 1e9;
      nem_torque = race::nemesis_rival_boost(true, true, int(n) - 1, other_ahead, player_prog - field_->progress(k),
                                             dist_to_fin, c.ai.braking(), c.ai.cornering())
                       .torque_add;
    }
    c.body->set_torque_factor(c.ai.torque() * (1 + boost + nem_torque));
    const bool over_corner = c.ai.state() == race::DriverAi::State::kRubberband && c.ai.mode() == 4;
    const auto& bp = c.body->params();
    const double half_track = 0.5 * std::fabs(bp.wheel_centres[1].x - bp.wheel_centres[0].x);
    const double com_h = std::max(0.20, bp.centre_of_mass.y);
    const double max_grip = (half_track > 0.3 && (half_track / com_h) < 1.45) ? 1.0 : 1.25;
    // Keep deliberate mode-4 over-cornering mistakes at 1.0 grip so they can run wide,
    // while allowing Hard/Pro AI and kCatchUp to retain their cornering grip up to max_grip.
    const double ai_corner_grip = over_corner ? 1.0 : std::clamp(c.ai.cornering(), 1.0, max_grip);
    c.body->set_ai_grip(ai_corner_grip, ai_corner_grip);
    const double d = std::hypot(at.x - cx, at.z - cz);
    const bool was_far = c.far;
    if (c.far && d < radius - 80) c.far = false;
    else if (!c.far && d > radius - 40) c.far = true;
    bool ribbon = c.far;
    if (!ribbon) {
      int grounded_wheels = 0;
      for (const auto& w : c.body->wheels())
        if (w.contact) ++grounded_wheels;
      // Only probe for collision holes when returning from far ribbon or losing wheel contact;
      // avoids 7 extra full-world BVH raycasts per 120 Hz step when cars are firmly grounded.
      if (was_far || grounded_wheels < 2) {
        double gy = 0;
        ribbon = !world::ground_height(near, at.x, at.z, at.y + 3, gy) || gy < at.y - 10;
        step_hole[k] = ribbon ? 1 : 0;
      }
    }
    step_ribbon[k] = ribbon ? 1 : 0;
    c.before = c.body->pose();
    c.body->step(in, ribbon ? static_cast<const drive::Ground&>(*ribbon_) : near);
    const drive::Vec3 np = c.body->position(), nv = c.body->velocity();
    if (!std::isfinite(np.x) || !std::isfinite(np.y) || !std::isfinite(np.z) || !std::isfinite(nv.x) ||
        !std::isfinite(nv.y) || !std::isfinite(nv.z)) {
      step_bad[k] = 1;
    }
  };
  if (step_pool_ && n >= 2) {
    step_pool_->run(n, step_ai_car);
  } else {
    for (size_t k = 0; k < n; ++k) step_ai_car(k);
  }
  for (size_t k = 0; k < n; ++k) {
    hole_steps_ += step_hole[k];
    far_steps_ += step_ribbon[k];
    if (step_bad[k]) reset_car(*cars_[k].body, k, near, cx, cz, radius);
    // Update persistent per-AI CarLampState (15-tick brake hold, staggered dusk/dawn
    // headlamps, per-car overhead tunnel roof probe, and Reverse gear == -1 mapped to gear == 0).
    if (cars_[k].body) {
      const bool tod_lamps = drive::CarLampState::tod_requests_headlamps(double(tod_sec_));
      if (!cars_[k].far && !tod_lamps) {
        if (cars_[k].tunnel_probe_tick == 0) {
          cars_[k].in_tunnel = is_under_tunnel_roof(&near, cars_[k].body->position());
          cars_[k].tunnel_probe_tick = 12;
        } else {
          --cars_[k].tunnel_probe_tick;
        }
      } else {
        cars_[k].in_tunnel = false;
        cars_[k].tunnel_probe_tick = 0;
      }
      drive::LampInputs lin;
      const float brk = (k < inputs_.size()) ? float(inputs_[k].brake) : 0.0f;
      lin.brake_cmd = brk;
      lin.brake_pedal = brk;
      const int vgear = cars_[k].body->gear();
      lin.gear = (vgear == -1) ? 0 : std::max(1, vgear);
      lin.tod_sec = double(tod_sec_);
      lin.dt = float(drive::Vehicle::kStep);
      lin.light_override = cars_[k].in_tunnel ? 1 : 0;
      lin.is_ai = true;
      lin.snap_headlamps = !cars_[k].lamps_initialized;
      cars_[k].lamps.update(lin);
      cars_[k].lamps_initialized = true;
    }
  }
  // Feed live Race AI opponents into Street/Nemesis TrafficView so traffic brakes/yields
  // for Race AI and promotes traffic near Race AI (>30 m from player) into physics bodies.
  if (traffic && n > 0) {
    std::vector<traffic::ExternalVehicle> ext;
    ext.reserve(n);
    for (size_t k = 0; k < n; ++k) {
      if (!cars_[k].body) continue;
      const drive::Vec3 pos = cars_[k].body->position();
      const drive::Vec3 vel = cars_[k].body->velocity();
      ext.push_back({{pos.x, pos.y, pos.z}, {vel.x, vel.y, vel.z}, true});
    }
    traffic->set_external_vehicles(std::move(ext));
  }
  // Contacts (PhysicsSettings.ini [Collision], SPEC-RACE-AI Q55):
  // PlayerTorqueScale = 0.0, RaceCarTorqueScaleInRace = 0.2,
  // RaceCarTorqueScaleVsTrafficInRace = 0.05, TrafficTorqueScale = 0.25,
  // restitution = 0.0 (CarCarResitution typo falls back to DefaultRestitution).
  constexpr double kPlayerTorqueScale = 0.0;
  constexpr double kRaceCarTorqueScale = 0.2;
  constexpr double kRaceCarVsTrafficTorqueScale = 0.05;
  drive::ContactOptions contact_options;
  contact_options.restitution = 0;
  contact_options.friction = 0.05;
  player_impulse_ = 0.0;
  player_impulse_local_pos_ = {0.0f, 0.5f, 1.5f};
  if (!rivals_solo_ && session_->collides(n)) {
    const drive::Quat pq = player.orientation();
    const drive::Quat pq_inv{-pq.x, -pq.y, -pq.z, pq.w};
    const float hw = float(std::max(0.5, 0.5 * player.params().size.x));
    const float hl = float(std::max(1.0, 0.5 * player.params().size.z));
    const float hh = float(std::max(0.8, player.params().size.y));
    for (size_t k = 0; k < n; ++k) {
      if (!session_->collides(k) || !cars_[k].body) continue;
      drive::Vehicle& ai_v = *cars_[k].body;
      const drive::Vec3 d = player.position() - ai_v.position();
      const double r = player.params().size.z + ai_v.params().size.z;
      if (!(d.x * d.x + d.z * d.z <= r * r)) continue;
      double j = 0.0;
      if (drive::car_contact(player, kPlayerTorqueScale, ai_v, kRaceCarTorqueScale, contact_options, &j)) {
        ++contact_count_;
        if (j > player_impulse_) {
          player_impulse_ = j;
          const drive::Vec3 rel = ai_v.position() - player.position();
          const drive::Vec3 loc = drive::rotate(pq_inv, rel);
          player_impulse_local_pos_ = {
              std::clamp(float(loc.x), -hw, hw),
              std::clamp(float(loc.y) + 0.5f, 0.1f, hh),
              std::clamp(float(loc.z), -hl, hl),
          };
        }
      }
    }
  }
  contacts_.clear();
  if (!rivals_solo_) {
    for (size_t k = 0; k < n; ++k)
      if (session_->collides(k))
        contacts_.push_back(
            {cars_[k].body.get(), kRaceCarTorqueScale, 0, drive::ContactRole::kRaceAi, kRaceCarVsTrafficTorqueScale});
  }
  if (traffic) traffic->contact_bodies(contacts_, 1);
  contact_count_ += drive::resolve_contacts(contacts_, contact_options);
  // The session: each car's move over the step.
  auto sample = [&](size_t i, const drive::Vehicle& v, double x0, double z0) {
    race::CarSample& s = samples_[i];
    const drive::Vec3 p = v.position();
    s.x0 = x0, s.z0 = z0, s.x1 = p.x, s.z1 = p.z;
    s.speed = v.speed();
    s.progress = field_->progress(i);
    const double cr = field_->cross_track(i);
    double h = 0.0, room = 1e9;
    line_heading_and_room_fast(line_.line, s.progress, cr > 0, h, room);
    const drive::AiCarState st = drive::ai_state(v);
    s.heading_cos = st.fwd_x * std::sin(h) + st.fwd_z * std::cos(h);
    s.off_route = std::max(0.0, std::fabs(cr) - room);
    s.up_y = v.up_y();
    s.y = p.y;
    // The AI reset rules count line points as the AI's waypoints (interim).
    s.waypoint = line_.line.size() > 1 ? double(line_.line.segment(s.progress, 0)) : -1.0;
  };
  for (size_t k = 0; k < n; ++k) sample(k, *cars_[k].body, cars_[k].px, cars_[k].pz);
  sample(n, player, ppx_, ppz_);
  // Checkpoints in order (SPEC-EVENTS, Pass rule); the last is the finish,
  // held until every earlier one is passed. A missed checkpoint triggers a
  // 4.0 s return-to-track countdown banner and auto-resets just before the gate.
  bool trigger_missed_cp_reset = false;
  double missed_cp_reset_progress = 0.0;
  if (checkpoint_gates_.size() > 1) {
    next_checkpoint_.resize(n + 1, 0);
    const size_t last = checkpoint_gates_.size() - 1;
    for (size_t i = 0; i <= n; ++i) {
      const auto& q = samples_[i];
      size_t& k = next_checkpoint_[i];
      // Player must cross checkpoint gate planes in order (SPEC-EVENTS); AI keeps
      // a +10 m line-progress fallback so off-line AI avoidance never deadlocks a race.
      const bool crossed = checkpoint_gates_[k].crossing(q.x0, q.z0, q.x1, q.z1) >= 0;
      const bool ai_fallback = (i < n) && (q.progress > checkpoint_progress_[k] + 10);
      const bool gate_passed = session_->started() && k < last && (crossed || ai_fallback);
      if (gate_passed) ++k;
      session_->hold_finish(i, k < last);
      if (i == n) {
        const bool racing_now = session_->started() && !session_->finished(n) && k < last;
        if (racing_now) {
          const auto& gate = checkpoint_gates_[k];
          const double gate_side = gate.side(q.x1, q.z1);
          const double gate_dist = std::hypot(q.x1 - gate.x, q.z1 - gate.z);
          if (missed_cp_.step(drive::Vehicle::kStep, true, gate_passed, k, q.progress, checkpoint_progress_[k],
                              gate_side, gate_dist, gate.reach, q.off_route)) {
            trigger_missed_cp_reset = true;
            missed_cp_reset_progress = std::max(line_.gate_progress, checkpoint_progress_[k] - 12.0);
          }
        } else {
          missed_cp_.reset();
        }
      }
    }
  } else {
    missed_cp_.reset();
  }
  session_->step(drive::Vehicle::kStep, samples_);
  if (trigger_missed_cp_reset) {
    reset_car_to_progress(player, n, missed_cp_reset_progress, near, cx, cz, radius);
  }
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
  showcase_rumble_ = 0.0;
  if (showcase_.active()) {
    const double near_r2 = radius > 0.0 ? radius * radius : 0.0;
    const double probe_top_y = std::max(showcase_.pose().position[1], player.position().y) + 400.0;
    constexpr double kProbeSpan = 750.0;
    auto ground_probe = [&](double px, double pz) -> std::optional<double> {
      double best_y = -1e30;
      bool found = false;
      const double dx = px - cx;
      const double dz = pz - cz;
      if (near_r2 <= 0.0 || (dx * dx + dz * dz) <= near_r2) {
        drive::Hit hit{};
        if (near.raycast({px, probe_top_y, pz}, {0.0, -1.0, 0.0}, kProbeSpan, hit)) {
          const double hy = probe_top_y - hit.distance;
          if (std::isfinite(hy)) {
            best_y = hy;
            found = true;
          }
        }
      }
      if (ribbon_) {
        drive::Hit rhit{};
        if (ribbon_->raycast({px, probe_top_y, pz}, {0.0, -1.0, 0.0}, kProbeSpan, rhit)) {
          const double ry = probe_top_y - rhit.distance;
          if (std::isfinite(ry) && (!found || ry > best_y)) {
            best_y = ry;
            found = true;
          }
        }
      }
      return found ? std::optional<double>(best_y) : std::nullopt;
    };
    showcase_.step(drive::Vehicle::kStep, session_->clock(), field_->progress(n), ground_probe);
    showcase_rumble_ = showcase_rumble_strength(player.position());
  }
  for (size_t i : session_->resets()) reset_car(i < n ? *cars_[i].body : player, i, near, cx, cz, radius);
  for (size_t i = 0; i <= n; ++i)
    if (session_->finished(i) && session_->car(i).finish > session_->clock() - drive::Vehicle::kStep - 1e-9)
      std::printf("race: t %.2f s: %s finished %s in %.3f s\n", session_->clock(), name_of(i).c_str(),
                  ordinal(session_->car(i).place), session_->car(i).finish);
  // Record player Rivals / Time-Attack ghost samples when enabled.
  if (record_ghost_ && session_->started()) {
    const auto& me = session_->car(n);
    const bool just_finished =
        session_->finished(n) && me.finish > session_->clock() - drive::Vehicle::kStep - 1e-9;
    if (!session_->finished(n) || just_finished) {
      const drive::Vec3 p = player.position();
      const drive::Quat q = player.orientation();
      race::GhostSample gs;
      gs.time = just_finished ? me.finish : session_->clock();
      gs.x = p.x, gs.y = p.y, gs.z = p.z;
      gs.qx = q.x, gs.qy = q.y, gs.qz = q.z, gs.qw = q.w;
      gs.speed = player.speed();
      gs.steer = float(player.steer_angle());
      gs.brake = float(player.shaped_input().brake);
      gs.gear = player.gear();
      recorded_ghost_.record(gs, just_finished);
      recorded_ghost_.clean = !me.race_dirty;
      if (just_finished) recorded_ghost_.total_time = me.finish;
    }
  }
  // A stage's splits (StartLine + (FinishLine - StartLine) * SplitPoints_k),
  // Co-Driver pace-note gate crossings, and stage finish ranking.
  if (stage_ && session_->clock() >= 0.0 && !session_->finished(n)) {
    const auto& pos = player.position();
    codriver_.observe(session_->clock(), pos.x, pos.z, std::fabs(player.speed()));
  }
  if (have_stage_track_ && stage_place_ == 0) {
    if (session_->finished(n)) {
      const double t = session_->car(n).finish;
      stage_place_ = race::rally_place(stage_field_, t);
      std::vector<double> sorted = stage_field_;
      std::sort(sorted.begin(), sorted.end());
      std::printf("race: stage time %.3f s: %d of %zu (field %.2f to %.2f s)\n", t, stage_place_,
                  stage_field_.size() + 1, sorted.empty() ? 0.0 : sorted.front(), sorted.empty() ? 0.0 : sorted.back());
    } else if (split_times_.size() < stage_track_.splits.size() && session_->clock() > 0) {
      const size_t k = split_times_.size();
      const double frac =
          stage_track_.start_line + (stage_track_.finish_line - stage_track_.start_line) * stage_track_.splits[k];
      if (field_->progress(n) >= line_.gate_progress + frac * stage_track_.length) {
        const double clk = session_->clock();
        split_times_.push_back(clk);
        double target_cum = 0.0;
        for (size_t j = 0; j <= k && j < stage_track_.best.size(); ++j) target_cum += stage_track_.best[j];
        last_split_delta_ = target_cum > 0.0 ? (clk - target_cum) : 0.0;
        last_split_at_ = clk;
        std::printf("race: t %.2f s: split %zu (delta %+.2f s)\n", clk, split_times_.size(), last_split_delta_);
      }
    }
  }
}

void RaceView::set_ghost(race::GhostRun run, Showroom* model, float alpha) {
  ghost_run_ = std::move(run);
  ghost_model_ = model;
  ghost_alpha_ = std::clamp(alpha, 0.05f, 1.0f);
  ghost_active_ = !ghost_run_.empty();
}

void RaceView::clear_ghost() {
  ghost_run_.clear();
  ghost_active_ = false;
  ghost_model_ = nullptr;
}

bool RaceView::ghost_sample(race::GhostSample& out) const {
  if (!ghost_active_ || ghost_run_.empty() || !session_ || !session_->started()) return false;
  return ghost_run_.interpolate(session_->clock(), out);
}

void RaceView::enable_ghost_recording(bool on) {
  record_ghost_ = on;
  if (on) {
    recorded_ghost_.clear();
    recorded_ghost_.event_id = !event_.horizon_id.empty() ? event_.horizon_id : ("stage:" + std::to_string(route_.id));
    recorded_ghost_.car_media = o_.player_car;
    if (player_pi_ > 0.0 && !bands_.empty()) recorded_ghost_.shown_pi = garage::shown_index(player_pi_, bands_);
  }
}

bool RaceView::build_rivals_ghost(double target_time_s, std::string_view rival_name, std::string_view car_media,
                                  int64_t car_id, int shown_pi, race::GhostRun& out) const {
  const auto& L = line_.line;
  if (L.dist.size() < 2) return false;
  const double start_p = line_.gate_progress;
  const double end_p = finish_progress_ > start_p + 50.0 ? finish_progress_ : L.length();
  const double span = std::max(50.0, end_p - start_p);
  const int laps = circuit_ && session_ ? std::max(1, session_->laps()) : 1;
  const double total_span = circuit_ ? (L.length() * double(laps)) : span;
  const int sample_n = std::clamp(int(std::ceil(total_span / 20.0)), 16, 4000);
  std::vector<race::GhostWaypoint> wps;
  wps.reserve(size_t(sample_n) + 1);
  const bool has_y = L.y.size() == L.size();
  double fallback_y = 0.0;
  if (!slots_.empty()) fallback_y = slots_.front().y;
  for (int i = 0; i <= sample_n; ++i) {
    const double s_rel = total_span * (double(i) / double(sample_n));
    const double s_line = circuit_ ? std::fmod(start_p + s_rel, std::max(1.0, L.length())) : (start_p + s_rel);
    auto it = std::lower_bound(L.dist.begin(), L.dist.end(), float(s_line));
    size_t idx = size_t(it - L.dist.begin());
    if (idx >= L.size()) idx = L.size() - 1;
    const size_t i0 = idx > 0 ? idx - 1 : 0;
    const double seg = double(L.dist[idx]) - double(L.dist[i0]);
    const double u = seg > 1e-5 ? std::clamp((s_line - double(L.dist[i0])) / seg, 0.0, 1.0) : 0.0;
    const double wx = L.x[i0] + u * (L.x[idx] - L.x[i0]);
    double wy = has_y ? (L.y[i0] + u * (L.y[idx] - L.y[i0])) : fallback_y;
    const double wz = L.z[i0] + u * (L.z[idx] - L.z[i0]);
    if (!has_y && ribbon_) {
      drive::Hit hit{};
      if (ribbon_->raycast({wx, fallback_y + 200.0, wz}, {0.0, -1.0, 0.0}, 400.0, hit)) {
        wy = (fallback_y + 200.0) - hit.distance;
        fallback_y = wy;
      }
    }
    wps.push_back({s_rel, wx, wy, wz});
  }
  const std::string eid = !event_.horizon_id.empty() ? event_.horizon_id : ("stage:" + std::to_string(route_.id));
  out = race::synthesize_target_ghost(wps, target_time_s, eid, rival_name, car_media, car_id, shown_pi, 0.1);
  return !out.empty();
}

void RaceView::instances(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                         std::vector<Showroom::Instance>& out) const {
  std::unordered_set<Showroom*> posed_models;
  if (!rivals_solo_) {
    for (size_t k = 0; k < cars_.size(); ++k) {
      const auto& c = cars_[k];
      if (!c.model || !c.body) continue;
      Showroom::Instance in;
      if (blend_ >= 1) {
        in = guest_instance(player_info, player, *c.model, *c.body);
      } else {
        const drive::Vehicle::Pose now = c.body->pose();
        c.body->set_pose(drive::Vehicle::blend(c.before, now, blend_));
        in = guest_instance(player_info, player, *c.model, *c.body);
        c.body->set_pose(now);
      }
      // Drive each opponent's dedicated Showroom wheel spin, steer, and suspension pose.
      if (posed_models.insert(c.model).second) {
        WheelPose wposes[4]{};
        for (int wi = 0; wi < 4; ++wi) {
          const auto& w = c.body->wheels()[size_t(wi)];
          wposes[wi].centre[0] = float(w.centre.x);
          wposes[wi].centre[1] = float(w.centre.y);
          wposes[wi].centre[2] = float(w.centre.z);
          wposes[wi].steer = float(w.steer + w.toe);
          wposes[wi].spin = float(w.spin);
          wposes[wi].omega = float(w.omega);
        }
        c.model->set_wheel_poses(wposes);
      }
      if (c.has_ai_paint) {
        in.paint_rgb[0] = c.ai_paint_rgb[0];
        in.paint_rgb[1] = c.ai_paint_rgb[1];
        in.paint_rgb[2] = c.ai_paint_rgb[2];
        in.has_paint = true;
      }
      in.lamps = c.lamps.vectors();
      in.has_lamps = true;
      out.push_back(in);
    }
  }
  // Rivals / Time-Attack translucent ghost car playback.
  race::GhostSample gs;
  if (ghost_sample(gs)) {
    Showroom* ghost_sr =
        ghost_model_ ? ghost_model_ : find_loaded_model(ghost_run_.car_media, -1, &posed_models);
    if (!ghost_sr && !ghost_model_) ghost_sr = find_loaded_model(ghost_run_.car_media);
    if (ghost_sr) {
      if (posed_models.insert(ghost_sr).second) {
        constexpr float kTwoPi = 6.28318530717958647692f;
        const auto& rest = ghost_sr->car().wheel_centres_body;
        const float radius = std::clamp(rest[0][1], 0.26f, 0.55f);
        const float omega = float(gs.speed) / radius;
        float spin = std::fmod(float(gs.time) * omega, kTwoPi);
        if (spin < 0.0f) spin += kTwoPi;
        WheelPose gw[4]{};
        for (int wi = 0; wi < 4; ++wi) {
          gw[wi].centre[0] = rest[size_t(wi)][0];
          gw[wi].centre[1] = rest[size_t(wi)][1];
          gw[wi].centre[2] = rest[size_t(wi)][2];
          gw[wi].steer = (wi < 2) ? gs.steer : 0.0f;
          gw[wi].spin = spin;
          gw[wi].omega = omega;
        }
        ghost_sr->set_wheel_poses(gw);
      }
      Showroom::Instance gin =
          guest_instance_pose(player_info, player, *ghost_sr, {gs.x, gs.y, gs.z}, {gs.qx, gs.qy, gs.qz, gs.qw});
      gin.fade_alpha = ghost_alpha_;
      drive::CarLampState gl;
      gl.headlamps_on = drive::CarLampState::tod_requests_headlamps(tod_sec_);
      gl.brake_on = gs.brake > 0.1f;
      gl.reverse_on = (gs.gear == -1);
      gin.lamps = gl.vectors();
      gin.has_lamps = true;
      out.push_back(gin);
    }
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
  // The event's name from the string tables (cached in open()); its id without one.
  h.event = cached_event_name_;
  switch (event_.style) {
    case 2: h.event_type = "STREET RACE"; break;
    case 3: h.event_type = "SHOWCASE"; break;
    case 5: h.event_type = "STAR SHOWDOWN"; break;
    case 6: h.event_type = "HORIZON FINALE"; break;
    default: h.event_type = "FESTIVAL RACE"; break;
  }
  h.circuit = circuit_;
  h.stage = have_stage_track_;
  h.clock = s.clock();
  h.digit = s.digit();
  h.wrong_way = wrong_way_ > 1.0;
  h.missed_checkpoint = missed_cp_.active;
  h.missed_checkpoint_countdown_s = missed_cp_.countdown_s;
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
  if (showcase_.active()) {
    h.showcase = true;
    h.showcase_aircraft = showcase_.spec().display_name;
    h.showcase_progress = showcase_.pose().fraction;
    h.showcase_gap_m = showcase_.player_gap_m(field_->progress(n));
    h.cars = 2;
    h.place = showcase_.player_place(field_->progress(n), s.finished(n), me.finish);
  }
  if (s.finished(n)) {
    h.phase = RaceHud::kFinished;
    h.finish_time = me.finish;
    h.base_credits = award_ ? award_->base_credits : credits_for(h.place);
    h.mileage_bonus = award_ ? award_->mileage_bonus : 0;
    h.difficulty_pct = award_ ? award_->difficulty_pct : race::total_difficulty_bonus_percent(int(o_.difficulty), o_.assists);
    h.difficulty_bonus = award_ ? award_->difficulty_bonus : 0;
    h.assists_pct = award_ ? award_->assists_pct : race::assist_difficulty_bonus_percent(o_.assists);
    h.assists_bonus = award_ ? award_->assists_bonus : 0;
    h.bounty_bonus = award_ ? award_->bounty_bonus : 0;
    h.skills_bonus = award_ ? award_->skills_bonus : 0;
    h.nemesis_bonus = award_ ? award_->nemesis_bonus : 0;
    h.driver_xp = award_ ? award_->driver_xp : 0;
    h.affinity_xp = award_ ? award_->affinity_xp : 0;
    h.credits = award_ ? award_->credits : credits_for(h.place);
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
      // The profile's name, as the original shows its gamertag: the
      // SHIFT_PLAYER_NAME override, else the OS account's name, else User.
      static const std::string me = [] {
        for (const char* v : {"SHIFT_PLAYER_NAME", "USERNAME", "USER"})
          if (const char* e = std::getenv(v); e && *e) return std::string(e).substr(0, 15);
        return std::string("User");
      }();
      r.name = me;
    } else {
      r.name = cars_[i].cached_driver_name;
    }
    r.place = int(h.rows.size()) + 1;
    r.finished = s.finished(i);
    r.time = s.car(i).finish;
    r.best_lap = s.car(i).best_lap;
    // Standings & results table car, class, and upgraded/faked PI.
    r.car = r.player ? cached_player_car_name_ : cars_[i].cached_car_name;
    const double raw = r.player ? player_pi_
                                : (cars_[i].upgraded_raw_pi > 0.0 ? cars_[i].upgraded_raw_pi : cars_[i].who.performance_index);
    if (!r.player && cars_[i].upgraded_shown_pi > 0 && !bands_.empty()) {
      r.pi = cars_[i].upgraded_shown_pi;
      r.car_class = bands_.back().name;
      for (const auto& b : bands_) {
        if (r.pi <= int(b.shown_max)) {
          r.car_class = b.name;
          break;
        }
      }
    } else if (raw > 0 && !bands_.empty()) {
      r.car_class = bands_[size_t(garage::class_of(raw, bands_))].name;
      r.pi = garage::shown_index(raw, bands_);
    }
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
    const int p = player_place();
    const int64_t credits = award_ ? award_->credits : credits_for(p);
    if (award_ && award_->wristband_points > 0)
      std::snprintf(b, sizeof b, "FINISHED %s PLACE  %s  %lld CR  +%d WRISTBAND", ordinal(p), t, (long long)credits,
                    award_->wristband_points);
    else std::snprintf(b, sizeof b, "FINISHED %s PLACE  %s  %lld CR", ordinal(p), t, (long long)credits);
  } else {
    clock_text(c, t, sizeof t);
    const char* go = c < 1.5 ? "GO!  " : "";
    const int p = player_place();
    const size_t total = showcase_.active() ? 2u : (n + 1);
    if (circuit_)
      std::snprintf(b, sizeof b, "%sLAP %d/%d  P%d/%zu  %s", go, s.lap(n), s.laps(), p, total, t);
    else {
      // A sprint, and any race on an open line: PROGRESS in percent of the
      // run (SPEC-EVENTS, Laps on an open route).
      const double run = std::max(1.0, finish_progress_ - line_.gate_progress);
      const double pc = std::clamp((field_->progress(n) - line_.gate_progress) / run, 0.0, 1.0);
      std::snprintf(b, sizeof b, "%sP%d/%zu  %s  PROGRESS %.0f%%", go, p, total, t, 100 * pc);
    }
  }
  return b;
}

int RaceView::player_place() const {
  if (!session_) return 1;
  const size_t pi = player_index();
  const auto& me = session_->car(pi);
  if (showcase_.active() && field_) {
    return showcase_.player_place(field_->progress(pi), session_->finished(pi), me.finish);
  }
  return me.place;
}

void RaceView::set_difficulty(race::Difficulty d) {
  o_.difficulty = d;
  if (showcase_.active() && session_ && !session_->started()) {
    const race::ShowcaseSpec spec =
        race::resolve_showcase_spec(event_.event_id, event_.horizon_id, showcase_xml_specs_, int(d));
    const double fin_p = finish_progress_ > line_.gate_progress + 100.0 ? finish_progress_ : line_.line.length();
    showcase_.init(spec, line_.line, line_.gate_progress, fin_p);
  }
}

double RaceView::showcase_rumble_strength(const drive::Vec3& player_pos) const {
  if (!showcase_.active()) return 0.0;
  const auto& sp = showcase_.pose().position;
  const double dx = sp[0] - player_pos.x;
  const double dy = sp[1] - player_pos.y;
  const double dz = sp[2] - player_pos.z;
  const double dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
  return showcase_.near_rumble_strength(dist_m);
}

Showroom* RaceView::find_loaded_model(const std::string& media, int64_t color_seq,
                                      const std::unordered_set<Showroom*>* exclude) const {
  if (media.empty() || models_.empty()) return nullptr;
  auto usable = [&](Showroom* sr) { return sr && (!exclude || !exclude->count(sr)); };
  if (color_seq >= 0) {
    const std::string exact = media + "#" + std::to_string(color_seq);
    if (auto it = models_.find(exact); it != models_.end() && usable(it->second.get())) return it->second.get();
    const std::string exact_slot = exact + "@";
    for (const auto& [k, m] : models_) {
      if (k.rfind(exact_slot, 0) == 0 && usable(m.get())) return m.get();
    }
  }
  const std::string prefix = media + "#";
  for (const auto& [k, m] : models_) {
    if (k.rfind(prefix, 0) == 0 && usable(m.get())) return m.get();
  }
  return nullptr;
}

int64_t RaceView::credits_for(int place) const {
  return race::race_credits(event_.cash_prize, race::winnings_for_career_type(event_.career_type),
                            int(session_->size()), place, true);
}

const race::RaceAward* RaceView::award(const race::CareerRecord& career, const race::WristbandTables& tables,
                                       double top_speed, const race::RaceBonusInputs& extras) {
  const size_t n = cars_.size();
  // A Rally stage pays nothing and writes no race result (SPEC-V4-CONTENT:
  // stages pay 0; championships, XP and their records are later work).
  if (stage_ || award_ || !session_->finished(n)) return nullptr;
  const auto& c = session_->car(n);
  const int racers = showcase_.active() ? 2 : int(session_->size());
  const int place = player_place();
  race::RaceBonusInputs eff_extras = extras;
  if (eff_extras.assists_pct == 0) eff_extras.assists_pct = race::assist_difficulty_bonus_percent(o_.assists);
  eff_extras.prize_car_max_place = std::max(eff_extras.prize_car_max_place, event_.prize_car_max_place);
  award_ = race::compute_race_award(career, event_.event_id, event_.career_type, event_.style, event_.level,
                                    event_.cash_prize, racers, place, true, event_.prize_car, tables,
                                    race::total_difficulty_bonus_percent(int(o_.difficulty), o_.assists), c.distance,
                                    eff_extras);
  race::RaceAwardCommit commit;
  commit.event_id = event_.event_id;
  commit.race_id = event_.race_id;
  commit.career_type = event_.career_type;
  commit.place = place;
  commit.finished = true;
  commit.race_time = c.finish;
  commit.top_speed = top_speed;
  pending_award_.hold(*award_, commit);
  return &*award_;
}

bool RaceView::showcase_tag(OpponentTag& out) const {
  if (!showcase_.active()) return false;
  const auto& p = showcase_.pose();
  out = OpponentTag{};
  out.x = p.position[0];
  out.y = p.position[1] + (showcase_.spec().kind == race::ShowcaseAircraftKind::kHotAirBalloon ? 11.5 : 3.0);
  out.z = p.position[2];
  out.speed = p.speed_mps;
  out.name = showcase_.spec().display_name;
  out.leader = !p.finished && field_ && p.progress_m > field_->progress(cars_.size());
  return true;
}

void RaceView::build_showcase_mesh(std::vector<Showroom::ShowcaseVertex>& out) const {
  if (!showcase_.active()) return;
  const auto& sp = showcase_.spec();
  const auto& pose = showcase_.pose();
  const double sc = sp.scale;
  const double cy = std::cos(pose.heading), sy = std::sin(pose.heading);
  const double cp = std::cos(pose.pitch), sp_p = std::sin(pose.pitch);
  const double cr = std::cos(pose.roll), sr = std::sin(pose.roll);
  constexpr double kPi = 3.14159265358979323846;

  // Local (+X right, +Y up, +Z forward) -> game world rotation (roll, pitch, yaw).
  auto xform_dir = [&](double lx, double ly, double lz) -> std::array<float, 3> {
    const double x1 = lx * cr - ly * sr;
    const double y1 = lx * sr + ly * cr;
    const double z1 = lz;
    const double x2 = x1;
    const double y2 = y1 * cp + z1 * sp_p;
    const double z2 = -y1 * sp_p + z1 * cp;
    const double wx = x2 * cy + z2 * sy;
    const double wy = y2;
    const double wz = -x2 * sy + z2 * cy;
    return {float(wx), float(wy), float(wz)};
  };
  auto norm3 = [](std::array<float, 3> v) -> std::array<float, 3> {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-6f ? std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l} : std::array<float, 3>{0, 1, 0};
  };
  auto xform_pt = [&](double lx, double ly, double lz) -> std::array<float, 3> {
    const auto d = xform_dir(lx * sc, ly * sc, lz * sc);
    return {float(pose.position[0]) + d[0], float(pose.position[1]) + d[1], float(pose.position[2]) + d[2]};
  };
  auto push_uv_tri = [&](const std::array<float, 3>& a, const std::array<float, 3>& na, const std::array<float, 2>& ua,
                         const std::array<float, 3>& b, const std::array<float, 3>& nb, const std::array<float, 2>& ub,
                         const std::array<float, 3>& c, const std::array<float, 3>& nc, const std::array<float, 2>& uc,
                         std::array<uint8_t, 4> col) {
    out.push_back({a, na, ua, col});
    out.push_back({b, nb, ub, col});
    out.push_back({c, nc, uc, col});
    out.push_back({a, {-na[0], -na[1], -na[2]}, ua, col});
    out.push_back({c, {-nc[0], -nc[1], -nc[2]}, uc, col});
    out.push_back({b, {-nb[0], -nb[1], -nb[2]}, ub, col});
  };
  auto push_flat_tri = [&](const std::array<float, 3>& a, const std::array<float, 2>& ua,
                           const std::array<float, 3>& b, const std::array<float, 2>& ub,
                           const std::array<float, 3>& c, const std::array<float, 2>& uc,
                           std::array<uint8_t, 4> col) {
    const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const auto n = norm3({uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx});
    push_uv_tri(a, n, ua, b, n, ub, c, n, uc, col);
  };
  // Smooth lofted fuselage/nacelle tube along +Z with elliptical cross-sections and proper cylindrical UVs.
  struct LoftRing {
    double z, yc, rx, ry, v;
  };
  auto add_loft = [&](const std::vector<LoftRing>& rings, int segs, std::array<uint8_t, 4> col) {
    for (size_t r = 0; r + 1 < rings.size(); ++r) {
      const auto& a = rings[r];
      const auto& b = rings[r + 1];
      for (int s = 0; s < segs; ++s) {
        const double th0 = 2.0 * kPi * double(s) / double(segs);
        const double th1 = 2.0 * kPi * double(s + 1) / double(segs);
        const float u0 = float(s) / float(segs), u1 = float(s + 1) / float(segs);
        const double c0 = std::cos(th0), s0 = std::sin(th0), c1 = std::cos(th1), s1 = std::sin(th1);
        const auto p00 = xform_pt(a.rx * c0, a.yc + a.ry * s0, a.z);
        const auto p10 = xform_pt(a.rx * c1, a.yc + a.ry * s1, a.z);
        const auto p11 = xform_pt(b.rx * c1, b.yc + b.ry * s1, b.z);
        const auto p01 = xform_pt(b.rx * c0, b.yc + b.ry * s0, b.z);
        const auto n0 = norm3(xform_dir(c0, s0, 0.0));
        const auto n1 = norm3(xform_dir(c1, s1, 0.0));
        push_uv_tri(p00, n0, {u0, float(a.v)}, p10, n1, {u1, float(a.v)}, p11, n1, {u1, float(b.v)}, col);
        push_uv_tri(p00, n0, {u0, float(a.v)}, p11, n1, {u1, float(b.v)}, p01, n0, {u0, float(b.v)}, col);
      }
    }
  };
  // Tapered swept airfoil wing / stabilizer panel (upper + lower cambered surfaces, UV mapped).
  auto add_wing = [&](double x_root, double x_tip, double y_root, double y_tip, double z_root_le, double z_root_te,
                      double z_tip_le, double z_tip_te, double thick_root, double thick_tip,
                      std::array<uint8_t, 4> col) {
    const auto r_le = xform_pt(x_root, y_root, z_root_le);
    const auto r_te = xform_pt(x_root, y_root, z_root_te);
    const auto r_top = xform_pt(x_root, y_root + thick_root, z_root_le * 0.65 + z_root_te * 0.35);
    const auto r_bot = xform_pt(x_root, y_root - thick_root * 0.55, z_root_le * 0.65 + z_root_te * 0.35);
    const auto t_le = xform_pt(x_tip, y_tip, z_tip_le);
    const auto t_te = xform_pt(x_tip, y_tip, z_tip_te);
    const auto t_top = xform_pt(x_tip, y_tip + thick_tip, z_tip_le * 0.65 + z_tip_te * 0.35);
    const auto t_bot = xform_pt(x_tip, y_tip - thick_tip * 0.55, z_tip_le * 0.65 + z_tip_te * 0.35);
    auto quad = [&](const std::array<float, 3>& p0, const std::array<float, 2>& u0,
                    const std::array<float, 3>& p1, const std::array<float, 2>& u1,
                    const std::array<float, 3>& p2, const std::array<float, 2>& u2,
                    const std::array<float, 3>& p3, const std::array<float, 2>& u3) {
      push_flat_tri(p0, u0, p1, u1, p2, u2, col);
      push_flat_tri(p0, u0, p2, u2, p3, u3, col);
    };
    quad(r_le, {0.1f, 0.1f}, t_le, {0.9f, 0.1f}, t_top, {0.9f, 0.5f}, r_top, {0.1f, 0.5f});
    quad(r_top, {0.1f, 0.5f}, t_top, {0.9f, 0.5f}, t_te, {0.9f, 0.9f}, r_te, {0.1f, 0.9f});
    quad(r_le, {0.1f, 0.1f}, r_bot, {0.1f, 0.5f}, t_bot, {0.9f, 0.5f}, t_le, {0.9f, 0.1f});
    quad(r_bot, {0.1f, 0.5f}, r_te, {0.1f, 0.9f}, t_te, {0.9f, 0.9f}, t_bot, {0.9f, 0.5f});
    push_flat_tri(t_le, {0.8f, 0.2f}, t_top, {0.9f, 0.5f}, t_te, {0.8f, 0.8f}, col);
    push_flat_tri(t_le, {0.8f, 0.2f}, t_te, {0.8f, 0.8f}, t_bot, {0.9f, 0.5f}, col);
  };
  auto add_fin = [&](double x0, double y_bot, double y_top, double z_bot_le, double z_bot_te, double z_top_le,
                     double z_top_te, double half_t, std::array<uint8_t, 4> col) {
    const auto bl = xform_pt(x0 - half_t, y_bot, z_bot_le), br = xform_pt(x0 + half_t, y_bot, z_bot_le);
    const auto bt = xform_pt(x0, y_bot, z_bot_te);
    const auto tl = xform_pt(x0 - half_t * 0.5, y_top, z_top_le), tr = xform_pt(x0 + half_t * 0.5, y_top, z_top_le);
    const auto tt = xform_pt(x0, y_top, z_top_te);
    push_flat_tri(bl, {0.1f, 0.9f}, bt, {0.9f, 0.9f}, tt, {0.9f, 0.1f}, col);
    push_flat_tri(bl, {0.1f, 0.9f}, tt, {0.9f, 0.1f}, tl, {0.1f, 0.1f}, col);
    push_flat_tri(br, {0.1f, 0.9f}, tr, {0.1f, 0.1f}, tt, {0.9f, 0.1f}, col);
    push_flat_tri(br, {0.1f, 0.9f}, tt, {0.9f, 0.1f}, bt, {0.9f, 0.9f}, col);
    push_flat_tri(bl, {0.05f, 0.9f}, tl, {0.05f, 0.1f}, tr, {0.15f, 0.1f}, col);
    push_flat_tri(bl, {0.05f, 0.9f}, tr, {0.15f, 0.1f}, br, {0.15f, 0.9f}, col);
  };
  auto add_strut = [&](double x0, double y0, double z0, double x1, double y1, double z1, double r,
                       std::array<uint8_t, 4> col) {
    const auto p0 = xform_pt(x0 - r, y0, z0), p1 = xform_pt(x0 + r, y0, z0);
    const auto p2 = xform_pt(x1 + r, y1, z1), p3 = xform_pt(x1 - r, y1, z1);
    push_flat_tri(p0, {0.05f, 0.05f}, p1, {0.1f, 0.05f}, p2, {0.1f, 0.95f}, col);
    push_flat_tri(p0, {0.05f, 0.05f}, p2, {0.1f, 0.95f}, p3, {0.05f, 0.95f}, col);
  };

  if (sp.kind == race::ShowcaseAircraftKind::kHotAirBalloon) {
    // Horizon Festival Hot-Air Balloon: smooth 20x12 UV-mapped envelope, rigging cables, burner & gondola.
    constexpr int kSegs = 20, kRings = 12;
    const std::array<uint8_t, 4> palette[4] = {
        {240, 25, 105, 255}, {255, 195, 20, 255}, {255, 105, 15, 255}, {245, 245, 250, 255},
    };
    auto env_r = [&](int r) {
      const double u = double(r) / kRings;
      return u < 0.18 ? 1.15 + u * 16.5 : 5.75 * std::sin(u * kPi * 0.90 + 0.22);
    };
    auto env_y = [&](int r) { return 3.0 + double(r) * 1.12; };
    for (int r = 0; r < kRings; ++r) {
      const double r0 = env_r(r), r1 = env_r(r + 1);
      const double y0 = env_y(r), y1 = env_y(r + 1);
      const float v0 = float(r) / float(kRings), v1 = float(r + 1) / float(kRings);
      for (int s = 0; s < kSegs; ++s) {
        const double a0 = 2.0 * kPi * s / kSegs, a1 = 2.0 * kPi * (s + 1) / kSegs;
        const float u0 = float(s) / float(kSegs), u1 = float(s + 1) / float(kSegs);
        const double c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        const auto p00 = xform_pt(r0 * c0, y0, r0 * s0);
        const auto p10 = xform_pt(r0 * c1, y0, r0 * s1);
        const auto p11 = xform_pt(r1 * c1, y1, r1 * s1);
        const auto p01 = xform_pt(r1 * c0, y1, r1 * s0);
        const auto n0 = norm3(xform_dir(c0, 0.18, s0));
        const auto n1 = norm3(xform_dir(c1, 0.18, s1));
        const auto col = palette[(s + r / 3) % 4];
        push_uv_tri(p00, n0, {u0, v0}, p10, n1, {u1, v0}, p11, n1, {u1, v1}, col);
        push_uv_tri(p00, n0, {u0, v0}, p11, n1, {u1, v1}, p01, n0, {u0, v1}, col);
      }
    }
    // Wicker gondola & burner glow + 8 suspension cables.
    add_loft({{-0.95, 0.7, 0.95, 0.72, 0.85}, {0.95, 0.7, 0.95, 0.72, 0.98}}, 8, {150, 102, 54, 255});
    add_loft({{-0.32, 2.35, 0.32, 0.42, 0.5}, {0.32, 2.35, 0.32, 0.42, 0.6}}, 8, {255, 196, 62, 255});
    for (int k = 0; k < 8; ++k) {
      const double a = 2.0 * kPi * k / 8.0;
      add_strut(0.88 * std::cos(a), 1.35, 0.88 * std::sin(a), 1.15 * std::cos(a), 3.05, 1.15 * std::sin(a), 0.04,
                {64, 66, 72, 255});
    }
    return;
  }

  if (sp.kind == race::ShowcaseAircraftKind::kHelicopter) {
    // Sculpted Attack Helicopter: tapered gunship hull, stepped tandem canopy, tail boom, skids & 4-blade rotor.
    const std::array<uint8_t, 4> body{56, 64, 72, 255}, accent{225, 68, 36, 255}, glass{28, 52, 78, 255},
        rotor{36, 38, 44, 255};
    add_loft({{-6.6, 0.35, 0.14, 0.18, 0.02},
              {-2.4, 0.25, 0.38, 0.42, 0.30},
              {-1.0, 0.15, 1.05, 0.92, 0.46},
              {1.4, 0.08, 1.12, 0.96, 0.68},
              {3.1, -0.05, 0.82, 0.62, 0.86},
              {4.2, -0.22, 0.26, 0.24, 0.98}},
             10, body);
    // Stepped cockpit canopy dome
    add_loft({{0.2, 0.72, 0.56, 0.38, 0.15}, {1.8, 0.68, 0.68, 0.44, 0.50}, {3.2, 0.32, 0.38, 0.24, 0.85}}, 8, glass);
    // Tail fin & stub weapon wings
    add_fin(0.0, 0.35, 1.95, -5.8, -6.9, -6.2, -7.1, 0.10, accent);
    add_wing(0.95, 2.85, -0.15, -0.30, 0.95, -0.15, 0.55, -0.25, 0.16, 0.09, accent);
    add_wing(-0.95, -2.85, -0.15, -0.30, 0.95, -0.15, 0.55, -0.25, 0.16, 0.09, accent);
    for (double sx : {-1.15, 1.15}) {
      add_strut(sx, -1.25, -1.6, sx, -1.25, 2.5, 0.09, rotor);
      add_strut(sx, -1.25, -0.5, sx * 0.75, -0.45, -0.3, 0.06, rotor);
      add_strut(sx, -1.25, 1.4, sx * 0.75, -0.45, 1.2, 0.06, rotor);
    }
    // Spinning 4-blade main rotor + rotor mast + tail rotor.
    add_strut(0.0, 0.95, 0.35, 0.0, 1.55, 0.35, 0.14, rotor);
    for (int b = 0; b < 4; ++b) {
      const double a = pose.rotor_angle + b * 0.5 * kPi;
      const double bx = std::cos(a) * 6.0, bz = std::sin(a) * 6.0;
      const double px = -std::sin(a) * 0.22, pz = std::cos(a) * 0.22;
      const auto p0 = xform_pt(px, 1.55, 0.35 + pz);
      const auto p1 = xform_pt(bx + px * 0.7, 1.55, 0.35 + bz + pz * 0.7);
      const auto p2 = xform_pt(bx - px * 0.7, 1.55, 0.35 + bz - pz * 0.7);
      const auto p3 = xform_pt(-px, 1.55, 0.35 - pz);
      push_flat_tri(p0, {0.05f, 0.1f}, p1, {0.95f, 0.1f}, p2, {0.95f, 0.9f}, rotor);
      push_flat_tri(p0, {0.05f, 0.1f}, p2, {0.95f, 0.9f}, p3, {0.05f, 0.9f}, rotor);
    }
    for (int b = 0; b < 2; ++b) {
      const double a = pose.rotor_angle * 2.4 + b * kPi;
      const double by = std::cos(a) * 0.95, bz = std::sin(a) * 0.95;
      add_strut(0.22, 1.25 - by, -6.65 - bz, 0.22, 1.25 + by, -6.65 + bz, 0.08, rotor);
    }
    return;
  }

  // Sculpted fixed-wing aircraft: P-51 Mustang, Stunt Jet, or Biplane.
  const bool is_jet = sp.kind == race::ShowcaseAircraftKind::kStuntJet;
  const bool is_biplane = sp.kind == race::ShowcaseAircraftKind::kBiplane;
  const std::array<uint8_t, 4> fuse = is_jet ? std::array<uint8_t, 4>{228, 232, 240, 255}
                                             : (is_biplane ? std::array<uint8_t, 4>{212, 36, 36, 255}
                                                           : std::array<uint8_t, 4>{216, 222, 230, 255});
  const std::array<uint8_t, 4> trim = is_jet ? std::array<uint8_t, 4>{245, 16, 98, 255}
                                             : (is_biplane ? std::array<uint8_t, 4>{246, 238, 214, 255}
                                                           : std::array<uint8_t, 4>{218, 30, 34, 255});
  const std::array<uint8_t, 4> canopy{32, 56, 84, 255};

  // Multi-ring tapered aerodynamic fuselage with proper UV wrap.
  add_loft({{-4.9, 0.12, 0.16, 0.28, 0.02},
            {-2.8, 0.05, 0.44, 0.56, 0.22},
            {-0.6, 0.00, 0.70, 0.78, 0.46},
            {1.8, 0.00, 0.72, 0.76, 0.68},
            {4.2, 0.00, 0.56, 0.58, 0.86},
            {5.3, 0.00, is_jet ? 0.06 : 0.34, is_jet ? 0.06 : 0.34, 0.96},
            {5.9, 0.00, 0.04, 0.04, 1.00}},
           12, fuse);
  // Sculpted bubble / teardrop canopy
  add_loft({{-0.7, 0.66, 0.32, 0.24, 0.15}, {0.4, 0.78, 0.44, 0.40, 0.50}, {1.6, 0.64, 0.28, 0.22, 0.85}}, 8, canopy);

  // Swept tapered cambered main wings (left & right).
  const double span = is_jet ? 5.9 : 6.7;
  const double z_tip_le = is_jet ? -1.4 : 0.95;
  const double z_tip_te = is_jet ? -3.2 : -0.15;
  add_wing(0.55, span, -0.16, is_jet ? -0.18 : 0.18, 1.9, -0.45, z_tip_le, z_tip_te, 0.24, 0.10, fuse);
  add_wing(-0.55, -span, -0.16, is_jet ? -0.18 : 0.18, 1.9, -0.45, z_tip_le, z_tip_te, 0.24, 0.10, fuse);
  if (is_biplane) {
    add_wing(0.0, 6.4, 1.38, 1.48, 1.95, 0.05, 1.55, 0.15, 0.18, 0.10, trim);
    add_wing(0.0, -6.4, 1.38, 1.48, 1.95, 0.05, 1.55, 0.15, 0.18, 0.10, trim);
    for (double sx : {-4.2, -2.0, 2.0, 4.2}) {
      add_strut(sx, -0.05, 1.25, sx, 1.42, 1.45, 0.06, {64, 66, 72, 255});
      add_strut(sx, -0.05, 0.35, sx, 1.42, 0.55, 0.06, {64, 66, 72, 255});
    }
  }
  // Tapered tailplane & vertical stabilizer(s)
  add_wing(0.18, 2.45, 0.14, 0.18, -3.5, -4.65, -4.05, -4.85, 0.12, 0.06, trim);
  add_wing(-0.18, -2.45, 0.14, 0.18, -3.5, -4.65, -4.05, -4.85, 0.12, 0.06, trim);
  if (is_jet) {
    add_fin(-0.78, 0.18, 1.95, -3.3, -4.8, -4.2, -5.0, 0.09, trim);
    add_fin(0.78, 0.18, 1.95, -3.3, -4.8, -4.2, -5.0, 0.09, trim);
    add_loft({{-5.4, 0.0, 0.36, 0.36, 0.1}, {-4.7, 0.0, 0.44, 0.44, 0.9}}, 10, {255, 148, 28, 255});
  } else {
    add_fin(0.0, 0.22, 2.15, -3.2, -4.75, -4.15, -4.95, 0.11, trim);
    if (!is_biplane) {
      // Iconic P-51D Mustang red spinner cone, ventral radiator scoop, and twin Merlin V12 exhaust manifolds.
      add_loft({{5.25, 0.0, 0.35, 0.35, 0.05}, {5.65, 0.0, 0.22, 0.22, 0.55}, {6.05, 0.0, 0.03, 0.03, 1.00}}, 10, trim);
      add_loft({{-1.75, -0.68, 0.28, 0.22, 0.05}, {-0.55, -0.78, 0.42, 0.30, 0.50}, {0.55, -0.72, 0.36, 0.26, 0.95}},
               8, fuse);
      const std::array<uint8_t, 4> exhaust_metal{54, 48, 44, 255};
      for (double sx : {-0.68, 0.68}) {
        add_strut(sx, 0.14, 2.55, sx, 0.14, 3.65, 0.08, exhaust_metal);
      }
    }
    // Spinning 4-blade (P-51) or 2-blade (Biplane) propeller with yellow safety tips.
    const int blades = is_biplane ? 2 : 4;
    const std::array<uint8_t, 4> prop{36, 38, 44, 255};
    const std::array<uint8_t, 4> prop_tip{236, 198, 42, 255};
    for (int b = 0; b < blades; ++b) {
      const double a = pose.rotor_angle + b * (2.0 * kPi / double(blades));
      const double bx = std::cos(a) * 1.92, by = std::sin(a) * 1.92;
      const double mx = bx * 0.84, my = by * 0.84;
      const double px = -std::sin(a) * 0.13, py = std::cos(a) * 0.13;
      const auto p0 = xform_pt(px, py, 5.55);
      const auto p1 = xform_pt(mx + px * 0.78, my + py * 0.78, 5.55);
      const auto p2 = xform_pt(mx - px * 0.78, my - py * 0.78, 5.55);
      const auto p3 = xform_pt(-px, -py, 5.55);
      push_flat_tri(p0, {0.05f, 0.1f}, p1, {0.80f, 0.1f}, p2, {0.80f, 0.9f}, prop);
      push_flat_tri(p0, {0.05f, 0.1f}, p2, {0.80f, 0.9f}, p3, {0.05f, 0.9f}, prop);
      const auto t1 = xform_pt(bx + px * 0.65, by + py * 0.65, 5.55);
      const auto t2 = xform_pt(bx - px * 0.65, by - py * 0.65, 5.55);
      push_flat_tri(p1, {0.80f, 0.1f}, t1, {0.95f, 0.1f}, t2, {0.95f, 0.9f}, prop_tip);
      push_flat_tri(p1, {0.80f, 0.1f}, t2, {0.95f, 0.9f}, p2, {0.80f, 0.9f}, prop_tip);
    }
  }
}

void RaceView::append_showcase_mesh(std::vector<world::DebrisVertex>& out) const {
  std::vector<Showroom::ShowcaseVertex> sv;
  build_showcase_mesh(sv);
  out.reserve(out.size() + sv.size());
  for (const auto& v : sv) out.push_back({v.pos, v.normal, v.rgba});
}

void RaceView::emit_showcase_vfx(world::ParticleSystem& particles, const world::ParticleLibrary& vfx_lib,
                                 const drive::Vec3& player_pos, float dt) const {
  if (!showcase_.active() || dt <= 0.0f) return;
  const float step_dt = std::clamp(dt, 0.0f, 0.05f);
  const auto& spec = showcase_.spec();
  const auto& pose = showcase_.pose();
  const double dx = pose.position[0] - player_pos.x;
  const double dy = pose.position[1] - player_pos.y;
  const double dz = pose.position[2] - player_pos.z;
  const double dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!std::isfinite(dist_m) || dist_m > 650.0) return;

  const float dist_scale = float(std::clamp(1.0 - (dist_m - 360.0) / 290.0, 0.15, 1.0));
  const auto anchors = race::compute_showcase_vfx_anchors(spec, pose);

  auto pick_effect = [&](std::initializer_list<const char*> cands) -> std::string_view {
    for (const char* name : cands) {
      if (vfx_lib.find_effect(name)) return name;
    }
    return {};
  };

  const std::array<float, 3> fwd{float(anchors.forward[0]), float(anchors.forward[1]), float(anchors.forward[2])};
  const std::array<float, 3> right{float(anchors.right[0]), float(anchors.right[1]), float(anchors.right[2])};
  const std::array<float, 3> up{float(anchors.up[0]), float(anchors.up[1]), float(anchors.up[2])};
  const std::array<float, 3> vel_exhaust{float(anchors.velocity[0] * 0.28), float(anchors.velocity[1] * 0.28),
                                         float(anchors.velocity[2] * 0.28)};
  const std::array<float, 3> vel_trail{float(anchors.velocity[0] * 0.16), float(anchors.velocity[1] * 0.16),
                                       float(anchors.velocity[2] * 0.16)};

  // 1. Twin Merlin V12 / turbine engine exhaust puffs along the fuselage flanks.
  const std::string_view exhaust_eff = pick_effect({"smoke", "amb_housesmoke", "amb_handflare_smoke", "smokecollision"});
  if (!exhaust_eff.empty() && anchors.exhaust_count > 0) {
    if (spec.kind == race::ShowcaseAircraftKind::kHotAirBalloon) {
      const std::array<float, 3> epos{float(anchors.exhaust_pos[0][0]), float(anchors.exhaust_pos[0][1]),
                                      float(anchors.exhaust_pos[0][2])};
      particles.emit_continuous(exhaust_eff, epos, {0.0f, 1.0f, 0.0f}, {0.0f, 1.2f, 0.0f}, step_dt,
                                0.22f * dist_scale, 0x8A0100ull);
    } else {
      const float speed_t = float(std::clamp(pose.speed_mps / 55.0, 0.40, 1.15));
      const float climb_boost = float(std::clamp(pose.pitch * 1.4, 0.0, 0.35));
      const float ex_inten = std::clamp((0.34f * speed_t + climb_boost * 0.24f) * dist_scale, 0.12f, 0.60f);
      for (int i = 0; i < anchors.exhaust_count; ++i) {
        const float side = (anchors.exhaust_count == 2) ? (i == 0 ? -1.0f : 1.0f) : 0.0f;
        const std::array<float, 3> epos{float(anchors.exhaust_pos[size_t(i)][0]),
                                        float(anchors.exhaust_pos[size_t(i)][1]),
                                        float(anchors.exhaust_pos[size_t(i)][2])};
        const std::array<float, 3> edir{-fwd[0] * 0.92f + right[0] * (side * 0.24f) - up[0] * 0.08f,
                                        -fwd[1] * 0.92f + right[1] * (side * 0.24f) - up[1] * 0.08f,
                                        -fwd[2] * 0.92f + right[2] * (side * 0.24f) - up[2] * 0.08f};
        particles.emit_continuous(exhaust_eff, epos, edir, vel_exhaust, step_dt, ex_inten, 0x8A0100ull + uint64_t(i));
      }
    }
  }

  // 2. Subtle translucent wingtip condensation / vortex trails during banked turns & high-speed pull-ups.
  if (anchors.wingtip_count == 2 && pose.speed_mps > 24.0) {
    const std::string_view vortex_eff = pick_effect({"smokerim0", "smoke", "amb_handflare_smoke"});
    if (!vortex_eff.empty()) {
      const float bank_t = float(std::clamp((std::fabs(pose.roll) - 0.10) / 0.48, 0.0, 1.0));
      const float pull_t = float(std::clamp((pose.pitch - 0.06) / 0.30, 0.0, 1.0));
      const float spd_t = float(std::clamp((pose.speed_mps - 24.0) / 36.0, 0.0, 1.0));
      const float vortex_inten = std::clamp((0.14f + 0.34f * std::max(bank_t, pull_t)) * spd_t * dist_scale, 0.0f, 0.44f);
      if (vortex_inten > 0.06f) {
        const std::array<float, 3> vdir{-fwd[0], -fwd[1], -fwd[2]};
        for (int i = 0; i < 2; ++i) {
          const std::array<float, 3> wpos{float(anchors.wingtip_pos[size_t(i)][0]),
                                          float(anchors.wingtip_pos[size_t(i)][1]),
                                          float(anchors.wingtip_pos[size_t(i)][2])};
          particles.emit_continuous(vortex_eff, wpos, vdir, vel_trail, step_dt, vortex_inten, 0x8A0200ull + uint64_t(i));
        }
      }
    }
  }

  // 3. Low-altitude prop-wash / rotor-wash dust swirl when skimming within ~22 m of ground near the player.
  if (spec.kind != race::ShowcaseAircraftKind::kHotAirBalloon && anchors.agl_m >= 0.5 && anchors.agl_m <= 22.0 &&
      dist_m < 280.0) {
    const std::string_view dust_eff = pick_effect({"dust1", "dirt1", "smoke"});
    if (!dust_eff.empty()) {
      const float wash_t = float(std::clamp((22.0 - anchors.agl_m) / 10.0, 0.0, 1.0));
      const float near_t = float(std::clamp(1.0 - dist_m / 280.0, 0.0, 1.0));
      const float wash_inten = std::clamp(wash_t * (0.38f + 0.42f * near_t), 0.0f, 0.78f);
      if (wash_inten > 0.05f) {
        const double swirl = pose.rotor_angle * 0.25;
        const float radius = float(2.6 + 2.0 * (1.0 - double(wash_t)));
        const float cs = float(std::cos(swirl));
        const float sn = float(std::sin(swirl));
        const float gy = float(anchors.ground_wash_pos[1]) + 0.35f;
        for (int i = 0; i < 2; ++i) {
          const float sgn = (i == 0) ? 1.0f : -1.0f;
          const float ox = sgn * cs;
          const float oz = sgn * sn;
          const std::array<float, 3> gpos{float(anchors.ground_wash_pos[0]) + ox * radius, gy,
                                          float(anchors.ground_wash_pos[2]) + oz * radius};
          const std::array<float, 3> gdir{ox * 0.68f - fwd[0] * 0.35f, 0.52f, oz * 0.68f - fwd[2] * 0.35f};
          particles.emit_continuous(dust_eff, gpos, gdir, {ox * 3.5f, 1.2f, oz * 3.5f}, step_dt, wash_inten,
                                    0x8A0300ull + uint64_t(i));
        }
      }
    }
  }
}

void RaceView::opponent_tags(std::vector<OpponentTag>& out) const {
  out.clear();
  if (!session_ || rivals_solo_) return;
  const auto ord = session_->order();
  const size_t lead_idx = ord.empty() ? cars_.size() : ord[0];
  out.reserve(cars_.size());
  for (size_t i = 0; i < cars_.size(); ++i) {
    if (!cars_[i].body) continue;
    const drive::Vec3 p = cars_[i].body->position();
    const drive::Vec3 v = cars_[i].body->velocity();
    out.push_back({p.x, p.y, p.z, cars_[i].body->speed(), cars_[i].cached_driver_name, i == lead_idx, v.x, v.y, v.z,
                   cars_[i].body->rpm(), cars_[i].who.car_id, cars_[i].who.car_media, cars_[i].cached_car_name});
  }
}

void RaceView::collect_replay_roster(std::vector<drive::ReplayCarInfo>& out) const {
  if (rivals_solo_) return;
  auto lin_to_srgb_u8 = [](float v) -> uint32_t {
    const float c = std::clamp(v, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    return uint32_t(std::clamp(std::lround(s * 255.0f), 0l, 255l));
  };
  for (size_t k = 0; k < cars_.size(); ++k) {
    const auto& c = cars_[k];
    drive::ReplayCarInfo ri;
    ri.slot = uint32_t(k + 1);
    ri.car_id = int32_t(c.who.car_id);
    ri.car_media = c.who.car_media;
    ri.driver_name = !c.cached_driver_name.empty() ? c.cached_driver_name : name_of(k);
    ri.is_player = false;
    ri.is_traffic = false;
    int64_t cseq = c.who.color_sequence;
    if (cseq <= 0) {
      if (const auto* col = event_.ai_player_color(c.who); col && col->color_sequence > 0) cseq = col->color_sequence;
    }
    ri.paint_index = cseq > 0 ? int32_t(cseq) : -1;
    if (c.has_ai_paint) {
      ri.has_body_color = true;
      ri.body_color = (lin_to_srgb_u8(c.ai_paint_rgb[0]) << 16) | (lin_to_srgb_u8(c.ai_paint_rgb[1]) << 8) |
                      lin_to_srgb_u8(c.ai_paint_rgb[2]);
    }
    out.push_back(std::move(ri));
  }
}

void RaceView::collect_replay_samples(std::vector<drive::ReplayCarSample>& out) const {
  if (rivals_solo_) return;
  if (replay_prev_gears_.size() != cars_.size()) {
    replay_prev_gears_.assign(cars_.size(), 1);
    for (size_t k = 0; k < cars_.size(); ++k) {
      if (cars_[k].body) replay_prev_gears_[k] = int8_t(std::clamp(cars_[k].body->gear(), -1, 10));
    }
  }
  for (size_t k = 0; k < cars_.size(); ++k) {
    const auto& c = cars_[k];
    if (!c.body) continue;
    const drive::Input inp = k < inputs_.size() ? inputs_[k] : drive::Input{};
    const uint8_t place = session_ ? uint8_t(std::clamp(session_->car(k).place, 0, 255)) : uint8_t(k + 1);
    drive::ReplayCarSample sample = drive::ReplayCarSample::from_vehicle(
        uint32_t(k + 1), *c.body, c.lamps, inp.throttle, inp.brake, inp.handbrake ? 1.0 : 0.0, inp.steer, place);
    if (sample.gear != replay_prev_gears_[k]) {
      sample.event_bits |= drive::kReplayEventGearShift;
      if (sample.gear < replay_prev_gears_[k] && sample.rpm > sample.redline_rpm * 0.65f) {
        sample.event_bits |= drive::kReplayEventBackfire;
      }
      replay_prev_gears_[k] = sample.gear;
    }
    const double contact_force = c.body->body_contact_force();
    if (contact_force > c.body->params().mass * 12.0) {
      sample.event_bits |= drive::kReplayEventImpact;
      const float dv = float(contact_force * drive::Vehicle::kStep / std::max(500.0, c.body->params().mass));
      sample.impact_intensity_u8 = uint8_t(std::clamp(std::lround((dv / 25.0f) * 255.0f), 16l, 255l));
    }
    out.push_back(sample);
  }
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

double RaceView::ai_reference_time() const {
  if (!session_ || cars_.empty()) return 0.0;
  double sum = 0.0;
  int count = 0;
  for (size_t k = 0; k < cars_.size(); ++k) {
    if (session_->finished(k)) {
      sum += session_->car(k).finish;
      ++count;
    }
  }
  if (count > 0) return sum / double(count);
  // Estimate from best AI progress if player finished first.
  const double clk = session_->clock();
  const double run = std::max(1.0, finish_progress_ - line_.gate_progress);
  if (clk > 1.0) {
    double best_frac = 0.0;
    for (size_t k = 0; k < cars_.size(); ++k) {
      const double frac = (field_->progress(k) - line_.gate_progress) / run;
      best_frac = std::max(best_frac, frac);
    }
    if (best_frac > 0.1) return clk / best_frac;
  }
  return 0.0;
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

RaceView::RewindSnapshot RaceView::capture_rewind() const {
  RewindSnapshot snap;
  if (session_) snap.session = session_->snapshot();
  if (field_) snap.field = field_->snapshot();
  snap.cars.reserve(cars_.size());
  for (const auto& c : cars_) {
    if (!c.body) continue;
    RewindSnapshot::AiCarState st(*c.body);
    st.before = c.before;
    st.ai = c.ai;
    st.start_progress = c.start_progress;
    st.px = c.px;
    st.pz = c.pz;
    st.far = c.far;
    st.traction_cap = c.traction_cap;
    st.lamps = c.lamps;
    st.lamps_initialized = c.lamps_initialized;
    snap.cars.push_back(std::move(st));
  }
  snap.player_ai = player_ai_;
  snap.next_checkpoint = next_checkpoint_;
  snap.lanes = lanes_;
  snap.ppx = ppx_;
  snap.ppz = ppz_;
  snap.wrong_way = wrong_way_;
  snap.split_times = split_times_;
  snap.last_split_delta = last_split_delta_;
  snap.last_split_at = last_split_at_;
  snap.stage_place = stage_place_;
  snap.has_showcase = showcase_.active();
  if (snap.has_showcase) snap.showcase = showcase_.snapshot();
  snap.award = award_;
  return snap;
}

void RaceView::restore_rewind(const RewindSnapshot& s, bool mark_player_lap_dirty) {
  player_impulse_ = 0.0;
  player_impulse_local_pos_ = {0.0f, 0.5f, 1.5f};
  showcase_rumble_ = 0.0;
  if (session_) {
    session_->restore(s.session);
    if (mark_player_lap_dirty) session_->mark_lap_dirty(player_index());
  }
  if (field_) field_->restore(s.field);
  size_t si = 0;
  for (auto& c : cars_) {
    if (!c.body) continue;
    if (si >= s.cars.size()) break;
    const auto& st = s.cars[si++];
    *c.body = st.body;
    c.before = st.before;
    c.ai = st.ai;
    c.start_progress = st.start_progress;
    c.px = st.px;
    c.pz = st.pz;
    c.far = st.far;
    c.traction_cap = st.traction_cap;
    c.lamps = st.lamps;
    c.lamps_initialized = st.lamps_initialized;
    c.in_tunnel = false;
    c.tunnel_probe_tick = 0;
  }
  player_ai_ = s.player_ai;
  next_checkpoint_ = s.next_checkpoint;
  lanes_ = s.lanes;
  ppx_ = s.ppx;
  ppz_ = s.ppz;
  wrong_way_ = s.wrong_way;
  missed_cp_.reset();
  split_times_ = s.split_times;
  last_split_delta_ = s.last_split_delta;
  last_split_at_ = s.last_split_at;
  stage_place_ = s.stage_place;
  if (s.has_showcase && showcase_.active()) showcase_.restore(s.showcase);
  award_ = s.award;
  last_planner_c_.assign(last_planner_c_.size(), -1.0f);
  last_planner_b_.assign(last_planner_b_.size(), -1.0f);
  if (session_) codriver_.rewind_to(session_->clock());
}

void RaceView::collect_rewind_actors(std::vector<drive::RewindActorPose>& out) const {
  for (size_t i = 0; i < cars_.size(); ++i) {
    const auto& c = cars_[i];
    if (!c.body) continue;
    const drive::Vehicle::Pose p = c.body->pose();
    drive::RewindActorPose ap{};
    ap.kind = 0u;
    ap.id = uint32_t(i);
    ap.pos = p.pos;  // centre of mass (matches Vehicle::Pose::pos)
    ap.rot = p.rot;
    ap.vel = p.vel;
    ap.steer = float(p.wheels[0].steer);
    ap.wheel_spin = float(p.wheels[0].spin);
    out.push_back(ap);
  }
  if (showcase_.active()) {
    const auto& sp = showcase_.pose();
    drive::RewindActorPose ap{};
    ap.kind = 3u;
    ap.id = 0u;
    ap.pos = {sp.position[0], sp.position[1], sp.position[2]};
    ap.rot = {0.0, std::sin(sp.heading * 0.5), 0.0, std::cos(sp.heading * 0.5)};
    const double cp = std::cos(sp.pitch);
    ap.vel = {std::sin(sp.heading) * cp * sp.speed_mps, std::sin(sp.pitch) * sp.speed_mps,
              std::cos(sp.heading) * cp * sp.speed_mps};
    ap.steer = float(sp.heading);
    ap.wheel_spin = float(sp.speed_mps);
    ap.extra0 = float(sp.pitch);
    ap.extra1 = float(sp.roll);
    ap.extra2 = float(sp.rotor_angle);
    out.push_back(ap);
  }
}

}  // namespace carview
