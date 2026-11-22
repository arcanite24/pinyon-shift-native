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
#include "garage/garage.h"
#include "race/difficulty.h"
#include "race/event.h"
#include "race/race_line.h"
#include "race/rally.h"
#include "race/rewards.h"
#include "race/route.h"
#include "race/session.h"
#include "game_hud.h"
#include "showroom.h"

namespace cars {
class PartsDb;
}

namespace world {
class Collision;
}

namespace carview {

class TrafficView;

class RaceView {
 public:
  struct Options {
    race::Difficulty difficulty = race::Difficulty::kMed;
    race::AiMode ai_mode = race::AiMode::kStock;
    race::DifficultyAssists assists{};
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
    // A Rally stage (SPEC-V4-CONTENT, Events and routes): the event
    // "stage:<route>" in this world folder (ColoradoDirt): its own route
    // file and loose AI line, the player alone on slot 00, one lap.
    std::filesystem::path stage_world;
    std::filesystem::path state_root;
    const cars::PartsDb* parts_db = nullptr;
    std::string language = "EN";
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
  void set_tod_sec(float tod_sec) { tod_sec_ = tod_sec; }
  // Frees the models (before their renderer goes).
  void release_models();
  // Ours (SPEC-RACE-AI leaves the room to the line): trims the line's room
  // on each side to the nearest wall of the collision (a face within 50
  // degrees of vertical, 0.6 m up) less a car's half width, so a car moved
  // off the line to pass does not run into a barrier inside the room.
  void clip_room(world::Collision& collision);

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
  // What the HUD shows of the race (game_hud.h), from the session.
  RaceHud hud_info() const;
  std::string results() const;  // one line a car, by place
  const race::Session& session() const { return *session_; }
  // The opponents' positions (x, z), for the maps.
  void opponent_positions(std::vector<std::array<double, 2>>& out) const;
  // Opponents' 3D world positions + names + leader flag (for overhead tags and skill detection).
  struct OpponentTag {
    double x = 0, y = 0, z = 0;
    double speed = 0;
    std::string name;
    bool leader = false;
    double vx = 0, vy = 0, vz = 0;
    double rpm = 1000;
  };
  void opponent_tags(std::vector<OpponentTag>& out) const;
  // Allows updating difficulty on the pre-race grid menu before the race starts.
  void set_difficulty(race::Difficulty d) { o_.difficulty = d; }
  race::Difficulty difficulty() const { return o_.difficulty; }
  void set_ai_mode(race::AiMode m) { o_.ai_mode = m; }
  race::AiMode ai_mode() const { return o_.ai_mode; }
  // Mean finish time (s) of AI opponents that have finished (or estimated from
  // race progress if still running), used for AISkillAdjustments dynamic skill.
  double ai_reference_time() const;
  void set_assists(const race::DifficultyAssists& a) { o_.assists = a; }
  const race::DifficultyAssists& assists() const { return o_.assists; }
  // The active racing line and player's progress along it (for the 3D driving/braking line).
  const drive::RacingLine& racing_line() const { return line_.line; }
  double player_progress() const { return field_ ? field_->progress(player_index()) : 0.0; }
  // The race's line (x, z) from 150 m behind the player to 600 m ahead,
  // up to the finish, for the minimap's route.
  void route_near(std::vector<std::array<double, 2>>& out) const;
  // The frame's fraction of a step past the last one: instances() draws
  // each car between its last two steps (1: as stepped).
  void set_blend(double alpha) { blend_ = alpha; }
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
    drive::Vehicle::Pose before;  // the body before its last step (drawn between, Vehicle::Pose)
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
  std::filesystem::path game_root_;
  float tod_sec_ = 12.0f * 3600.0f;
  Options o_;
  race::EventData event_;
  race::CarNames names_;
  race::Route route_;
  // The class bands and the player's raw index, for the results table.
  std::vector<garage::ClassBand> bands_;
  double player_pi_ = -1;
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
  double player_start_ = 0, finish_progress_ = 0, stage_finish_progress_ = 0;
  double ppx_ = 0, ppz_ = 0;
  double wrong_way_ = 0;  // s the player has driven against the line (ours)
  bool circuit_ = true;
  bool street_ = false;                   // a line built along the road map (no AI line)
  bool stage_ = false;                    // a Rally stage (Options::stage_world)
  // The stage's track row, the field's times and the player's splits
  // (SPEC-V4-CONTENT; race/rally.h).
  bool have_stage_track_ = false;
  race::StageTrack stage_track_;
  std::vector<double> stage_field_, split_times_;
  int stage_place_ = 0;
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
  double blend_ = 1;
  struct ReplayRow {
    double t, x, y, z, heading, speed;
  };
  std::vector<ReplayRow> replay_;
  std::optional<race::RaceAward> award_;
  // The player's credits for place p (the spec's share; before the award).
  int64_t credits_for(int place) const;
};

}  // namespace carview
