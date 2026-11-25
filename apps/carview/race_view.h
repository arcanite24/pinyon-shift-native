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
#include <unordered_set>
#include <vector>

#include "drive/ai_driver.h"
#include "drive/contact.h"
#include "drive/lamps.h"
#include "drive/vehicle.h"
#include "garage/garage.h"
#include "race/difficulty.h"
#include "race/event.h"
#include "race/race_line.h"
#include "race/rally.h"
#include "race/rally_codriver.h"
#include "race/reset_rules.h"
#include "race/rewards.h"
#include "race/rivals.h"
#include "race/route.h"
#include "race/session.h"
#include "race/showcase.h"
#include "game_hud.h"
#include "showroom.h"

namespace drive {
struct ReplayCarInfo;
struct ReplayCarSample;
struct RewindActorPose;
}

namespace cars {
class PartsDb;
struct PhysicsSettings;
}

namespace garage {
struct Car;
struct ClassBand;
struct PiConfig;
}

namespace world {
class Collision;
class ParticleLibrary;
class ParticleSystem;
}

namespace carview {

class TrafficView;

// Resolves the raw PerformanceIndex (0..1] for the active player car so Race AI
// scales to the player's fitted garage upgrades (P0-7). Returns < 0 when the
// driven car is a loaner / non-garage override so RaceView uses stock PI.
double resolve_player_race_pi(const garage::Car* selected_garage_car, const std::string& active_car_media,
                              const std::map<int64_t, std::string>& baked_ids, const cars::PartsDb* db,
                              const cars::PhysicsSettings* phys_settings, const garage::PiConfig* pi_cfg,
                              const std::vector<garage::ClassBand>* bands);

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
    std::filesystem::path rally_package;
    std::filesystem::path state_root;
    std::string stage_title;  // optional display name for Rally stage / championship stage
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
  // car does (held until the signal with player throttle pass-through for
  // launch revving; the AI's input under autopilot or once finished; else `user`).
  drive::Input player_input(const drive::Vehicle& player, const drive::Input& user,
                            const TrafficView* traffic = nullptr);
  // Pre-start 3-2-1 countdown grid hold input: preserves player throttle so
  // the player can rev, build turbo boost, and modulate launch RPM while
  // holding the car stationary in 1st gear until GO.
  static drive::Input countdown_grid_input(const drive::Input& user, bool autopilot = false, double clock_s = -6.0);
  // One-shot trigger set on event open/restart so callers reset non-Discount-Sign smashables.
  bool consume_smashables_reset() {
    const bool v = smashables_reset_pending_;
    smashables_reset_pending_ = false;
    return v;
  }
  // Appends compact 20 Hz actor poses (Race AI cars + Showcase aircraft) for smooth rewind scrub.
  void collect_rewind_actors(std::vector<drive::RewindActorPose>& out) const;
  // CareerRaceModes.xml UseTraffic="true": true for all Street & Nemesis races.
  bool uses_traffic() const { return race::event_uses_traffic(event_.career_type); }
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
  // Once the player has finished: the race's award computed from `career`
  // (SPEC-EVENT-REWARDS) and held, once a race; null before then or when
  // already computed. Nothing is written to `career` until commit_award
  // (the original updates the profile after the finish banner).
  // `top_speed` m/s for the counters.
  const race::RaceAward* award(const race::CareerRecord& career, const race::WristbandTables& tables,
                               double top_speed, const race::RaceBonusInputs& extras = {});
  // Applies the held award to `career` once; null when none is held.
  const race::RaceAward* commit_award(race::CareerRecord& career) { return pending_award_.commit(career); }
  // Whether an award is computed but not committed.
  bool award_pending() const { return pending_award_.held(); }
  // Forgets the held award (Restart, Quit).
  void drop_award() { pending_award_.drop(); }
  // The player has finished and the award, if the race pays one, has been
  // committed (not dropped by a Restart or Quit): the post-race screens
  // after the finish banner may run.
  bool post_race_settled() const { return player_finished() && (stage_ || pending_award_.committed()); }
  // Opponents' 3D world positions + names + leader flag (for overhead tags, skill detection, and Horizon Promo).
  struct OpponentTag {
    double x = 0, y = 0, z = 0;
    double speed = 0;
    std::string name;
    bool leader = false;
    double vx = 0, vy = 0, vz = 0;
    double rpm = 1000;
    int64_t car_id = 0;
    std::string car_media;
    std::string car_name;
  };
  // Collects full Race AI opponent metadata & per-frame samples (quaternion, wheels, lamps, audio/VFX events) for replays.
  void collect_replay_roster(std::vector<drive::ReplayCarInfo>& out) const;
  void collect_replay_samples(std::vector<drive::ReplayCarSample>& out) const;
  // Airborne Showcase support (PLANE_RACE_001..007): textured 3D aircraft mesh & sky tag.
  bool is_showcase() const { return showcase_.active(); }
  const race::ShowcaseOpponent& showcase() const { return showcase_; }
  double showcase_rumble() const { return showcase_rumble_; }
  double showcase_rumble_strength(const drive::Vec3& player_pos) const;
  void build_showcase_mesh(std::vector<Showroom::ShowcaseVertex>& out) const;
  void append_showcase_mesh(std::vector<world::DebrisVertex>& out) const;
  void emit_showcase_vfx(world::ParticleSystem& particles, const world::ParticleLibrary& vfx_lib,
                         const drive::Vec3& player_pos, float dt) const;
  bool showcase_tag(OpponentTag& out) const;
  // Car-to-car collision impulse against race AI during the last step (N s) & player-local contact point.
  double player_impulse() const { return player_impulse_; }
  std::array<float, 3> player_impulse_local_pos() const { return player_impulse_local_pos_; }
  // Effective player race place (airborne showcase returns 1 or 2 vs aircraft; normal race returns session place).
  int player_place() const;
  // Reuses an already-uploaded opponent guest Showroom for replay without synchronous re-upload hitches.
  Showroom* find_loaded_model(const std::string& media, int64_t color_seq = -1,
                              const std::unordered_set<Showroom*>* exclude = nullptr) const;
  const race::EventData& event() const { return event_; }
  std::string banner() const;
  // What the HUD shows of the race (game_hud.h), from the session.
  RaceHud hud_info() const;
  std::string results() const;  // one line a car, by place
  const race::Session& session() const { return *session_; }
  // The opponents' positions (x, z), for the maps.
  void opponent_positions(std::vector<std::array<double, 2>>& out) const;
  void opponent_tags(std::vector<OpponentTag>& out) const;
  // R3-UI-04: Missed checkpoint state for HUD warning & countdown.
  bool missed_checkpoint() const { return missed_cp_.active; }
  double missed_checkpoint_countdown_s() const { return missed_cp_.countdown_s; }
  // R3-UI-12: Minimap race markers (next unpassed checkpoint, checkered finish line, and AI race leader).
  bool next_checkpoint_pos(std::array<double, 2>& out) const;
  bool finish_pos(std::array<double, 2>& out) const;
  bool leader_opponent_pos(std::array<double, 2>& out) const;
  // Allows updating difficulty on the pre-race grid menu before the race starts.
  void set_difficulty(race::Difficulty d);
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
  int player_checkpoints_passed() const {
    const size_t pi = player_index();
    return pi < next_checkpoint_.size() ? int(next_checkpoint_[pi]) : 0;
  }
  // The race's line (x, z) from 150 m behind the player to 600 m ahead,
  // up to the finish, for the minimap's route.
  void route_near(std::vector<std::array<double, 2>>& out) const;
  // The frame's fraction of a step past the last one: instances() draws
  // each car between its last two steps (1: as stepped).
  void set_blend(double alpha) { blend_ = alpha; }
  bool player_finished() const { return session_->finished(player_index()); }
  size_t player_index() const { return cars_.size(); }
  bool is_stage() const { return stage_; }
  const race::StageTrack& stage_track() const { return stage_track_; }
  const std::vector<double>& stage_field() const { return stage_field_; }
  const std::vector<double>& split_times() const { return split_times_; }
  double last_split_delta() const { return last_split_delta_; }
  double last_split_at() const { return last_split_at_; }
  int stage_place() const { return stage_place_; }
  double stage_finish_time() const { return session_ && session_->finished(player_index()) ? session_->car(player_index()).finish : -1.0; }
  race::RallyPaceNoteTracker& codriver() { return codriver_; }
  const race::RallyPaceNoteTracker& codriver() const { return codriver_; }
  // Rivals / Time-Attack ghost playback & recording hooks.
  void set_ghost(race::GhostRun run, Showroom* model = nullptr, float alpha = 0.42f);
  void set_ghost_model(Showroom* model) { ghost_model_ = model; }
  void clear_ghost();
  bool has_ghost() const { return ghost_active_ && !ghost_run_.empty(); }
  const race::GhostRun& ghost_run() const { return ghost_run_; }
  bool ghost_sample(race::GhostSample& out) const;
  void enable_ghost_recording(bool on);
  const race::GhostRun& recorded_ghost() const { return recorded_ghost_; }
  // Synthesizes a target-tier `.shgh` ghost along this event's racing line finishing in `target_time_s`.
  bool build_rivals_ghost(double target_time_s, std::string_view rival_name, std::string_view car_media,
                          int64_t car_id, int shown_pi, race::GhostRun& out) const;
  // Solo Rivals / Time Trials mode: hides AI opponents & disables AI-vs-player collision so the player races the ghost 1v1.
  void set_rivals_solo(bool on) { rivals_solo_ = on; }
  bool rivals_solo() const { return rivals_solo_; }
  std::string summary() const;

  // Full race-world rewind snapshot & restore (P0-4): captures every AI rival
  // vehicle & controller, race/rally clock & Session state, AiField progress,
  // checkpoints, splits, showcase aircraft, and restores them atomically.
  struct RewindSnapshot {
    struct AiCarState {
      drive::Vehicle body;
      drive::Vehicle::Pose before{};
      race::DriverAi ai{};
      double start_progress = 0.0;
      double px = 0.0, pz = 0.0;
      bool far = false;
      float traction_cap = 1.0f;
      drive::CarLampState lamps{};
      bool lamps_initialized = false;
      AiCarState(const drive::Vehicle& v) : body(v) {}
    };
    race::Session::Snapshot session;
    drive::AiField::Snapshot field;
    std::vector<AiCarState> cars;
    race::DriverAi player_ai{};
    std::vector<size_t> next_checkpoint;
    std::vector<float> lanes;
    double ppx = 0.0, ppz = 0.0, wrong_way = 0.0;
    std::vector<double> split_times;
    double last_split_delta = 0.0, last_split_at = -1e9;
    int stage_place = 0;
    bool has_showcase = false;
    race::ShowcaseOpponent::Snapshot showcase{};
    std::optional<race::RaceAward> award;
  };
  RewindSnapshot capture_rewind() const;
  void restore_rewind(const RewindSnapshot& s, bool mark_player_lap_dirty);

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
    int target_shown_pi = 0;     // faked target shown PI when UpgradeAI is active
    int upgraded_shown_pi = 0;   // solved/upgraded shown PI displayed on HUD & standings
    double upgraded_raw_pi = 0;  // solved/upgraded raw PI
    bool has_ai_paint = false;   // AIPlayerColors sRGB->linear override when ColorSequence == 0
    float ai_paint_rgb[3] = {1.0f, 1.0f, 1.0f};
    drive::CarLampState lamps{}; // persistent per-AI lamp state (15-tick brake hold & headlamp stagger)
    bool lamps_initialized = false;
    bool in_tunnel = false;      // per-opponent overhead roof probe for tunnel headlamps
    uint8_t tunnel_probe_tick = 0;
    std::string cached_driver_name;
    std::string cached_car_name;
  };
  void reset_car(drive::Vehicle& v, size_t i, const drive::Ground& near, double cx, double cz, double radius);
  void reset_car_to_progress(drive::Vehicle& v, size_t i, double progress, const drive::Ground& near, double cx,
                             double cz, double radius);
  std::string name_of(size_t i) const;

  std::filesystem::path bake_dir_;
  std::filesystem::path game_root_;
  float tod_sec_ = 12.0f * 3600.0f;
  Options o_;
  race::EventData event_;
  race::CarNames names_;
  race::Route route_;
  // The class bands, PI virtual-lap config, and the player's raw index.
  std::vector<garage::ClassBand> bands_;
  garage::PiConfig pi_cfg_;
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
  race::HandOfGodReset hand_of_god_;      // GameTunableSettings.ini [HandOfGodReset]
  bool stage_ = false;                    // a Rally stage (Options::stage_world)
  // The stage's track row, the field's times and the player's splits
  // (SPEC-V4-CONTENT; race/rally.h).
  bool have_stage_track_ = false;
  race::StageTrack stage_track_;
  std::vector<double> stage_field_, split_times_;
  double last_split_delta_ = 0.0, last_split_at_ = -1e9;
  int stage_place_ = 0;
  race::RallyPaceNoteTracker codriver_;
  std::vector<race::Gate> checkpoint_gates_;  // a street race's checkpoints in order
  std::vector<size_t> next_checkpoint_;       // each car's next checkpoint (the player last)
  std::vector<double> checkpoint_progress_;   // each checkpoint's distance along the line
  MissedCheckpointTracker missed_cp_{};       // R3-UI-04: missed checkpoint countdown & reset tracker
  std::vector<race::RouteTransform> slots_;
  // Per step, every car (opponents, then the player).
  std::vector<drive::AiCarState> states_;
  std::vector<drive::Input> inputs_;
  std::vector<double> progress_;
  std::vector<float> cross_, speed_, lanes_, caps_;
  std::vector<race::LaneParams> lane_params_;
  std::vector<race::CarSample> samples_;
  std::vector<drive::ContactBody> contacts_;
  // Cached HUD/tag strings & planner parameters to avoid per-step lookups and cache invalidations.
  std::string cached_event_name_;
  std::string cached_player_car_name_;
  std::vector<float> last_planner_c_;
  std::vector<float> last_planner_b_;
  // Reusable per-step scratch buffers (zero heap allocations on the 120 Hz step path).
  std::vector<race::TrafficObstacle> traffic_obs_scratch_;
  std::vector<drive::DraftCar> draft_pool_scratch_;
  std::vector<drive::DraftCar> draft_leaders_scratch_;
  std::vector<double> draft_leader_strength_scratch_;
  std::vector<drive::DraftState> draft_follower_states_scratch_;
  std::vector<uint8_t> step_ribbon_scratch_, step_hole_scratch_, step_bad_scratch_;
  int contact_count_ = 0, resets_ = 0, far_steps_ = 0, hole_steps_ = 0;
  double player_impulse_ = 0.0;
  std::array<float, 3> player_impulse_local_pos_{0.0f, 0.5f, 1.5f};
  double showcase_rumble_ = 0.0;
  std::vector<race::ShowcaseSpec> showcase_xml_specs_;
  double cx_ = 0, cz_ = 0, radius_ = 0;
  bool center_known_ = false;
  double blend_ = 1;
  struct ReplayRow {
    double t, x, y, z, heading, speed;
  };
  std::vector<ReplayRow> replay_;
  std::optional<race::RaceAward> award_;
  race::PendingRaceAward pending_award_;
  race::ShowcaseOpponent showcase_;
  // Active Rivals / Time-Attack ghost run and optional recording buffer.
  race::GhostRun ghost_run_;
  bool ghost_active_ = false;
  float ghost_alpha_ = 0.42f;
  Showroom* ghost_model_ = nullptr;
  bool record_ghost_ = false;
  race::GhostRun recorded_ghost_;
  bool rivals_solo_ = false;
  mutable std::vector<int8_t> replay_prev_gears_;
  // Persistent worker pool for parallel multi-car AI physics steps (zero per-step spawn cost).
  struct PhysicsStepPool;
  std::unique_ptr<PhysicsStepPool> step_pool_;
  bool smashables_reset_pending_ = false;
  // The player's credits for place p (the spec's share; before the award).
  int64_t credits_for(int place) const;
};

}  // namespace carview
