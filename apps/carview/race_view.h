// carview --drive --event <id>: an event's race (SPEC-EVENTS, SPEC-RACE-AI)
// beside the player's car. The event's route, grid and opponents from the
// disc; each opponent its own car (physics and model from the bake) driven
// on the route's AI line with its skill, rubber band and lane; the session
// counts down, gates laps or the sprint finish, places the cars and keeps
// the results. Cars outside the gathered collision drive on the AI line's
// ribbon (the road the line describes), so the whole field keeps moving.
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "drive/ai_driver.h"
#include "drive/contact.h"
#include "drive/vehicle.h"
#include "race/difficulty.h"
#include "race/event.h"
#include "race/race_line.h"
#include "race/rewards.h"
#include "race/route.h"
#include "race/session.h"
#include "showroom.h"

namespace carview {

class TrafficView;

class RaceView {
 public:
  struct Options {
    race::Difficulty difficulty = race::Difficulty::kMed;
    bool autopilot = false;   // the player's car under the AI (headless runs)
    bool rubberband = true;   // false: the "no rubber-banding" option
    uint64_t seed = 1;
    // The M5 gate's replay (SPEC-RACE-AI Tests): the player's car follows an
    // oracle path by race time (CSV t, x, y, z, heading deg, ..., speed m/s;
    // the specification side's export), so the rubber bands see the
    // oracle's gaps. Empty: no replay.
    std::filesystem::path replay;
    // The player's car for the AI upgrades (SPEC-RACE-AI, AI car upgrades):
    // its raw PerformanceIndex, or < 0 to take `player_car`'s stock row.
    std::string player_car;
    double player_pi = -1;
  };
  explicit RaceView(std::filesystem::path bake_dir);
  ~RaceView();

  // The event's data: route, line, grid and opponents.
  bool open(const std::filesystem::path& game_root, const std::string& event, const Options& o, std::string& error);
  // The player's slot pose: x, y, z, heading.
  void player_spawn(double out[4]) const;
  // Where the player stands after the race (the route's
  // post_race_location_00, SPEC-EVENTS); false when the route has none.
  bool post_race_spawn(double out[4]) const;
  // Loads the opponents' cars (models and physics) and stands them on their
  // slots on `ground`.
  bool load_cars(Renderer& renderer, const drive::Ground& ground, std::string& error);
  // Frees the models (before their renderer goes).
  void release_models();

  // Before the player's step: the field's pass; returns what the player's
  // car does (held until the signal; the AI's input under autopilot or
  // once finished; else `user`).
  drive::Input player_input(const drive::Vehicle& player, const drive::Input& user);
  // After the player's step: the opponents' steps (on `near` inside the
  // gathered collision, around (cx, cz) within `radius`, else on the
  // ribbon), contacts, the session, rubber bands and resets.
  void after_step(drive::Vehicle& player, const drive::Ground& near, double cx, double cz, double radius,
                  TrafficView* traffic);

  // After the player's step, under --race-replay: the car put on the oracle
  // path at the race clock (held on the grid before the signal, driven by
  // the AI once the path ends).
  void replay_player(drive::Vehicle& player) const;

  void instances(const cars::bake::CarInfo& player_info, const drive::Vehicle& player,
                 std::vector<Showroom::Instance>& out) const;
  // Once the player has finished: the race's award applied to `career`
  // (SPEC-EVENT-REWARDS), once a race; null before then or when already
  // awarded this call. `top_speed` m/s for the counters.
  const race::RaceAward* award(race::CareerRecord& career, const race::WristbandTables& tables, double top_speed);
  const race::EventData& event() const { return event_; }
  std::string banner() const;
  std::string results() const;  // one line a car, by place
  const race::Session& session() const { return *session_; }
  // The opponents' positions (x, z), for the maps.
  void opponent_positions(std::vector<std::array<double, 2>>& out) const;
  bool player_finished() const { return session_->finished(player_index()); }
  size_t player_index() const { return cars_.size(); }
  std::string summary() const;

 private:
  struct Car {
    race::Opponent who;
    race::DriverSetup setup;
    race::DriverAi ai;
    int slot = 0;
    std::unique_ptr<drive::Vehicle> body;
    Showroom* model = nullptr;
    double start_progress = 0;
    double px = 0, pz = 0;  // before the step
    bool far = false;
    float traction_cap = 1;  // drive::ai_traction's state
    drive::AiCapacity capacity;  // the speed planner's tables (SPEC-RACE-AI)
    double upgrade_t = 0;        // AI car upgrades: > 0 up, < 0 down (fraction of the range)
  };
  void reset_car(drive::Vehicle& v, size_t i, const drive::Ground& near, double cx, double cz, double radius);
  std::string name_of(size_t i) const;

  std::filesystem::path bake_dir_;
  Options o_;
  race::EventData event_;
  race::Route route_;
  race::AiLineFile owt_;
  race::RaceLine line_;
  std::unique_ptr<race::RibbonGround> ribbon_;
  std::unique_ptr<drive::AiField> field_;
  std::unique_ptr<race::Session> session_;
  std::vector<Car> cars_;
  std::map<std::string, std::unique_ptr<Showroom>> models_;
  race::DriverAi player_ai_;
  drive::AiCapacity player_capacity_;
  bool player_capacity_known_ = false;
  // SHIFT_RACE_PLANNER=fit: the old flat fit (corner 1.7 g x C, braking
  // 2.0 g x B) for comparison; else the spec's planner.
  bool planner_fit_ = false;
  bool plan_dumped_ = false;  // SHIFT_PLAN_DUMP written
  int player_slot_ = 0;
  double player_start_ = 0, finish_progress_ = 0;
  double ppx_ = 0, ppz_ = 0;
  bool circuit_ = true;
  bool street_ = false;                   // a line built along the road map (no AI line)
  std::vector<race::Gate> checkpoint_gates_;  // a street race's checkpoints in order
  std::vector<size_t> next_checkpoint_;       // each car's next checkpoint (the player last)
  std::vector<double> checkpoint_progress_;   // each checkpoint's distance along the line
  std::vector<race::RouteTransform> slots_;
  // Per step, every car (opponents, then the player).
  std::vector<drive::AiCarState> states_;
  std::vector<drive::Input> inputs_;
  std::vector<double> progress_;
  std::vector<float> cross_, speed_, lanes_, caps_;
  std::vector<race::LaneParams> lane_params_;
  std::vector<race::CarSample> samples_;
  std::vector<drive::ContactBody> contacts_;
  int contact_count_ = 0, resets_ = 0, far_steps_ = 0, hole_steps_ = 0;
  double cx_ = 0, cz_ = 0, radius_ = 0;
  bool center_known_ = false;
  struct ReplayRow {
    double t, x, y, z, heading, speed;
  };
  std::vector<ReplayRow> replay_;
  std::optional<race::RaceAward> award_;
  // The player's credits for place p (the spec's share; before the award).
  int64_t credits_for(int place) const;
};

}  // namespace carview
